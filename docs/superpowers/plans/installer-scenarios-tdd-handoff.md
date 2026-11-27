# Installer scenarios TDD handoff

## Scope

This handoff records Tasks I1/I2 chronologically. The initial RED-only phase added the test-first contract in:

- `windows/native/tests/wix-installer-scenario-tests.ps1`
- `docs/superpowers/plans/installer-scenarios-tdd-handoff.md`

In that initial phase, no production harness, UI, MSI, transaction, bundle, workflow, build, or installer was changed, and tests were not run. Later implementation, independent Root verification, and remaining runtime limits are recorded below; the initial-phase restrictions are not a claim about the current implementation.

The test script is host-safe: it only defines pure-fixture objects, reads the sibling harness, and creates/removes files below one GUID-named directory under the current user's temporary directory. It never starts or stops a process, probes the host/registry, or writes outside that unique root. All generated evidence is labeled `synthetic_contract_only`; installer invocations remain zero.

## Root RED

Run from the repository root with Windows PowerShell 5.1 or PowerShell 7:

```powershell
& .\windows\native\tests\wix-installer-scenario-tests.ps1
```

Because production is intentionally absent, the first and only feature failure must be:

```text
[SC000] Missing installer scenario harness: ...\windows\native\tests\wix-installer-scenarios.ps1. Implement -ContractOnly before running these tests.
```

This is the authorized RED. Do not substitute a parser, PowerShell-version, missing-tool, permission, installer, registry, or fixture failure. Do not implement a production stub to make this check pass.

## Single runtime entry

The production implementer creates exactly:

```text
windows/native/tests/wix-installer-scenarios.ps1
```

Runtime invocation:

```powershell
pwsh -NoProfile -File .\windows\native\tests\wix-installer-scenarios.ps1 `
  -BaselineSetup <exe> `
  -CandidateSetup <exe> `
  -InputManifest <json> `
  -OutputDir <new-directory> `
  -Scenario Upgrade|Reentry|Recovery
```

`-InputManifest` is mandatory for runtime. `-OutputDir` must not exist. `-ContractOnly` is a non-runtime parameter set used only by tests or a caller that dot-sources the script:

```powershell
. .\windows\native\tests\wix-installer-scenarios.ps1 -ContractOnly
```

`-ContractOnly` must define the four functions below and then return without touching installers, processes, registry, services, the network, or the filesystem.

## Exact contract API

### 1. Preflight

```powershell
Assert-WixInstallerScenarioPreflight `
  -BaselineSetup <file> `
  -CandidateSetup <file> `
  -OutputDir <absent-path> `
  -Scenario Upgrade|Reentry|Recovery `
  -InputManifest <json> `
  -GuestObservation <object>
```

The manifest is the only trusted source of expected setup hashes and versions. Do not discover or silently replace it from a neighboring build. Its schema is:

```json
{
  "schema_version": 1,
  "owned_instance": {
    "authorization_kind": "owned-windows-sandbox-v1",
    "sandbox_id": "<explicit-owned-id>",
    "sentinel_path": "<guest-local-path>",
    "sentinel_sha256": "<sha256>"
  },
  "baseline": {
    "setup_sha256": "<sha256>",
    "runtime_version": "x.y.z",
    "burn_version": "x.y.z",
    "msi_version": "x.y.z"
  },
  "candidate": {
    "setup_sha256": "<sha256>",
    "runtime_version": "x.y.z",
    "burn_version": "x.y.z",
    "msi_version": "x.y.z"
  }
}
```

`GuestObservation` has:

```text
authorization_kind
sandbox_id
is_windows_sandbox_guest
sentinel_present
sentinel_path
sentinel_sha256
```

Runtime must populate this only from the explicitly supplied owned-instance authorization, guest-local Sandbox ID authorization, and guest-local sentinel. The preflight must reject:

- a host or merely detected VM (`SC010`);
- a missing/mismatched owned sentinel, authorization kind, or Sandbox ID (`SC010`);
- a missing/malformed manifest, missing setup, or setup hash mismatch (`SC011`);
- non-increasing runtime, Burn, or MSI versions (`SC012`);
- an existing output directory (`SC013`).

Candidate Burn/MSI versions must also agree with each other. The preflight returns an object with `scenario`, `baseline`, `candidate`, resolved input paths, and authorization evidence. It must not create `OutputDir`.

The committed tests and code must not contain the live instance ID, current artifact hashes, or a production manifest. Those belong only in the Root-supplied guest manifest.

### 2. Raw-log conversion

```powershell
ConvertFrom-WixInstallerScenarioLogs `
  -Scenario Upgrade|Reentry|Recovery `
  -BurnLogs @(@{ run='first|second'; text=<raw-log> }, ...) `
  -MsiLogs @(@{ run='first|second'; text=<raw-log> }, ...) `
  -Readback <object> `
  -EvidenceClass runtime_observed|synthetic_contract_only
```

Entries retain raw logs; `run` must be explicit. The parser must recognize bracketed ISO-8601 event times, `Apply begin`, `Apply complete, result:`, Burn shutdown/exit status, native lock evidence (`0x80070652`, `ERROR_INSTALL_ALREADY_RUNNING`, or `1618`), an MSI `InstallFinalize` success, MSI successful completion, Burn version, and MSI `ProductVersion`.

The returned evidence has flat fields including:

```text
scenario, evidence_class
first_apply_start_utc, first_apply_end_utc, first_apply_result, first_run_exit_code
second_invocation_start_utc, second_invocation_end_utc
second_native_lock_evidence, second_hresult
first_msi_transactions, second_msi_transactions
first_msi_commits, second_msi_commits
burn_version, msi_product_version, readback
```

Missing markers must remain unknown/false; they must not be converted to success. `first` and `second` MSI counts are derived from raw supplied logs, not from file names or a claimed count alone. Every unit fixture passes `synthetic_contract_only`.

### 3. Verdict

```powershell
Get-WixInstallerScenarioVerdict `
  -Scenario Upgrade|Reentry|Recovery `
  -Evidence <parser-result>
```

The return object always includes:

```text
scenario
verdict              # PASS | NOT_COVERED | FAIL
evidence_class
receipt_status
actual_scenario_pass
reason
```

For `synthetic_contract_only`, `receipt_status` is exactly `SYNTHETIC_CONTRACT_ONLY` and `actual_scenario_pass` is always false, even when the pure classifier returns `verdict=PASS`. Only `runtime_observed` evidence may make an actual scenario PASS.

Decision rules are intentionally narrow:

| Scenario | PASS | NOT_COVERED | FAIL |
| --- | --- | --- | --- |
| `Upgrade` | first Apply and MSI commit succeeded; expected runtime/Burn/MSI/payload/COM versions and old-registration absence agree | evidence is insufficient, with no contradictory result | contradictory state or a recorded first-run failure |
| `Reentry` | the second run overlaps the first Apply interval, explicit 1618/0x80070652/native-lock evidence exists, `second_msi_transactions=0`, and first run succeeds | no lock hit or no overlap after at most one targeted retry | a second MSI transaction, first-run failure, or more than one targeted retry |
| `Recovery` | a real new MSI commit plus bundle inconsistency occurred, normal double-click reran with zero passive/repair arguments, state converged, old registrations are absent, uninstall succeeded, and settings plus Doctor were preserved | injection never formed the MSI-committed/inconsistent state, including direct-MSI-only evidence, after at most two attempts | formed-state recovery failed or injection attempts exceeded two |

`Recovery` must additionally require `readback.injection_origin = 'candidate_ba_ui'`; direct-MSI state is supplemental only and can never close issue 16. A formed state followed by a non-normal rerun is a failure, not a PASS.

### 4. Kill eligibility

```powershell
Test-WixInstallerScenarioKillEligible `
  -ExpectedTarget <identity-captured-at-discovery> `
  -ObservedProcess <current-observation>
```

Both identity objects use:

```text
path
sha256
pid
creation_time_utc
```

The observed object additionally carries `process_kind` and `elevated`. Eligibility requires one exact candidate-BA-UI role, exact path/hash/PID/creation time, and `elevated=false`. It rejects:

- a name-only process observation;
- any identity mismatch or missing field;
- `msiexec`;
- the Windows Installer service;
- an elevated Burn engine;
- any host process.

The function returns `{ eligible, reason }`; it must not terminate anything. The runtime may stop a process only when this predicate is true immediately before the single bounded BA-only action, and must recheck the identity.

## Receipt and retry boundaries

Runtime receipts must preserve raw logs and separate Burn lock rejection, MSI transaction count, injection outcome, and final readback. They must emit:

```text
SYNTHETIC_CONTRACT_ONLY
```

for any classifier fixture. They must not write `scenario=... outcome=PASS` for synthetic evidence. Actual `PASS` is allowed only when `evidence_class=runtime_observed` and every scenario rule above is supported by retained evidence.

`Reentry` permits at most one timing-correction retry; another attempt is `FAIL`. `Recovery` permits at most two targeted candidate-BA injections; no qualifying state after two is `NOT_COVERED`. A recovery attempt that sees only an elevated engine, a service, `msiexec`, or a host is not eligible injection and must stop without killing it.

## Completion boundary

The next authorized step after the RED phase was Root's `SC000` run. The RED-only handoff left the existing `mapped_module_fixture.cpp` unchanged; the separately authorized GREEN extension below adds its optional bounded hold while preserving the old interface.

## GREEN implementation handoff

The harness is now implemented as:

```text
windows/native/tests/wix-installer-scenarios.ps1
```

The four pure API contracts remain source-visible under `-ContractOnly`. That parameter set returns immediately before any runtime helper definition or top-level file, process, registry, or environment action.

The existing mapped fixture keeps its two- and three-argument behavior and 45-second default. Its optional fourth argument is a decimal hold lifetime:

```text
mapped-module-fixture.exe <dll> [ready-file] [hold-seconds, 1..600]
```

`wix-installer-state-fixture.cpp` is a test fixture only. It seeds settings plus one disabled synthetic Doctor record under the interactive account, refuses any existing settings/Doctor/marker on `--seed`, and refuses `--read` without the exact synthetic marker. The harness compiles it from the supplied guest toolchain; it can instead use manifest-pinned fixture executables.

### Unit GREEN command

Root's command is still:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\windows\native\tests\wix-installer-scenario-tests.ps1
```

This remains synthetic-contract-only. The expected success line is:

```text
wix_installer_scenario_contracts_passed=<n>; evidence=synthetic_contract_only; installer_runs=0; process_actions=0; registry_actions=0
```

It was not executed during this implementation turn.

### Guest fixture manifest extensions

The previously documented manifest fields remain required. Runtime additionally accepts:

```json
{
  "fixtures": {
    "mapped_module_exe": "<guest-local-exe>",
    "mapped_module_exe_sha256": "<sha256>",
    "state_fixture_exe": "<guest-local-exe>",
    "state_fixture_exe_sha256": "<sha256>"
  },
  "candidate": {
    "ba_payloads": [
      {
        "sha256": "<trusted-extracted-BA-file-sha256>",
        "path": "<optional guest-local extracted file>"
      }
    ]
  }
}
```

`fixtures` can be omitted when `-ToolchainBin` is supplied; the harness compiles both fixtures inside the new output directory. `candidate.ba_payloads` is required for Recovery and must contain the extracted candidate BA image identities. The setup image hash is classified as Burn engine evidence and is never a fallback BA hash. Any `path` in `ba_payloads` is independently hash-checked before it can authorize an in-tree process.

No live Sandbox ID, production setup hash, BA hash, or artifact path is committed in the harness or fixtures.

### Runtime commands

Run serially in the one authorized guest, always from the normal `WDAGUtilityAccount` PowerShell session. Upgrade and Reentry start from the manifest's trusted baseline. Recovery can start from that baseline or from the candidate left by the preceding scenario: its preparation phase uses the same candidate EXE through the ordinary uninstall UI, reinstalls the baseline, and proves the clean readback before injection.

```powershell
$repo = 'C:\PolicyWork\repo'
$manifest = 'C:\PolicyOutput\scenario-inputs.json'
$baseline = 'C:\PolicyOutput\baseline.exe'
$candidate = 'C:\PolicyOutput\candidate.exe'
$toolchain = 'C:\PolicyWork\mingw64\bin'

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File "$repo\windows\native\tests\wix-installer-scenarios.ps1" `
  -BaselineSetup $baseline `
  -CandidateSetup $candidate `
  -InputManifest $manifest `
  -OutputDir C:\PolicyOutput\scenario-upgrade-20261127-1 `
  -Scenario Upgrade `
  -ToolchainBin $toolchain

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File "$repo\windows\native\tests\wix-installer-scenarios.ps1" `
  -BaselineSetup $baseline `
  -CandidateSetup $candidate `
  -InputManifest $manifest `
  -OutputDir C:\PolicyOutput\scenario-reentry-20261127-1 `
  -Scenario Reentry `
  -ToolchainBin $toolchain

powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File "$repo\windows\native\tests\wix-installer-scenarios.ps1" `
  -BaselineSetup $baseline `
  -CandidateSetup $candidate `
  -InputManifest $manifest `
  -OutputDir C:\PolicyOutput\scenario-recovery-20261127-1 `
  -Scenario Recovery `
  -ToolchainBin $toolchain
```

Recovery is deliberately launched from a non-elevated shell. Root must operate the displayed ordinary UI when `state.json` says `manual_normal_ui_required` or `starting_normal_candidate_ui`. The initial injection run carries only an explicit `/log <new-output-path>` argument; the ordinary recovery rerun and final ordinary uninstall carry zero installer arguments. The harness never automates clicks, UAC, text entry, SendKeys, or UI selection.

`-ObservationTimeoutSeconds` defaults to 900 and is bounded to 60–1800. `-MappedModuleHoldSeconds` defaults to 600 and is bounded to 1–600. The loaded fixture remains an independent, naturally bounded process; the harness never kills it.

### Runtime evidence and stop boundary

Each new output directory receives:

- `state.json`, atomically rewritten by the harness as phase/progress changes;
- `input-proof.json` with the supplied manifest hash and setup identities;
- `before.json`, `after.json`, or `converged.json` with the MSI-owned fixed root, the manifest's dynamic file count plus every payload hash/size, COM `--status`, actual COM server path/hash, MSI API inventory, and MSI/Burn uninstall-registry readback;
- `mapped-module.json` for the exact old DLL path/hash/PID/creation/module proof;
- raw copied Burn/MSI logs and their output hashes;
- `recovery-attempts.json` and `formed-state.json` when Recovery reaches that stage;
- final `receipt.json`, or explicit `failure.json`.

The `Recovery` stop predicate is rechecked against fresh ancestry from the held setup process and the held target process immediately before the exact PID action. Only a non-elevated image in that freshly verified candidate process tree, whose path/hash/PID/creation time exactly match, and whose hash is in the extracted-BA allowlist can be stopped. The setup hash is explicitly classified as Burn engine and cannot qualify. `msiexec`, the Windows Installer service, elevated engines, arbitrary host processes, ambiguous identities, or name-only observations are never eligible.

An MSI commit is not accepted from completion text alone. The relevant raw log must contain successful `InstallFinalize` and successful MSI completion. Recovery additionally requires the candidate MSI state plus an observed inconsistent Burn bundle state before ordinary rerun evidence is accepted.

Runtime exit codes are: `0` only for actual observed `PASS`, `2` for completed `NOT_COVERED`, and `1` for `FAIL` or a captured harness error. A synthetic unit `PASS` still serializes `SYNTHETIC_CONTRACT_ONLY` and can never become `actual_scenario_pass=true`.

### Intentionally not run or claimed

Root's `SC000` RED is accepted as authorization for this GREEN implementation. No test, build, fixture compilation, installer, process, registry, UI, network, WSL, or git operation was run by the implementer. Native GREEN, real loaded-DLL retirement/3010 behavior, native reentry lock coverage, BA extraction identity completeness, BA interruption timing, ordinary UI recovery, uninstall, and retention therefore remain runtime observations for Root. A `NOT_COVERED` receipt is the intended bounded result when no qualifying BA state forms; it does not close issue 16.

### Parser-only correction

The Root parser failure was corrected without changing contract or runtime semantics: native-capture stdout/stderr conditionals now assign scalar variables before constructing the result object, and command expressions in the discovered-log root array are explicitly parenthesized. The script begins with a UTF-8 BOM for Windows PowerShell 5.1. No test or parser executable was run in this phase; Root must independently re-run the parser and pure-contract RED/GREEN checks before authorizing the separately queued semantic regressions.

### Round 6 contract regressions

The semantic-test phase changes only `windows/native/tests/wix-installer-scenario-tests.ps1`. Root's parser and prior harness fixes remain untouched, and no production or fixture change is included.

The synthetic happy readback now keeps version domains separate:

- `expected_runtime_version`, `observed_runtime_version`, `observed_payload_version`, and `observed_com_version` use the product runtime version;
- `expected_msi_version`, raw `burn_version`, and raw `msi_product_version` use the MSI/Burn version.

Upgrade and Recovery also require explicit `true` observations for `manifest_integrity_ok`, `registration_status_ok`, `new_bundle_registered`, `settings_preserved`, and `doctor_preserved`. A present contradictory version or an explicit `false` observation is `FAIL`; a missing observation is `NOT_COVERED`. The runtime projection remains a separate implementation responsibility, and payload cardinality must come from the actual installed manifest rather than a universal fixed count of 12.

Additional table-driven cases cover:

- Burn timestamps with `Z`/offset behavior unchanged and ISO timestamps without an offset interpreted as **local time then converted to UTC** for overlap;
- an old MSI install/removal log not satisfying the candidate commit, a candidate begin-only log not committing, candidate `InstallFinalize` plus install status 3010 committing, and removal success not counting as install success;
- missing/empty extracted-BA payloads or a BA hash equal to the setup hash rejecting `SC034`, while one distinct extracted payload hash is accepted;
- explicit `burn_engine`, elevated, service, name-only, and identity-mismatched processes remaining ineligible.

`Require-Contract` records failures and continues so the script exposes the complete bounded RED. It throws once at the end as `SC900` with the failure count and at most 16 detail lines; parser/binder and missing-harness failures still stop immediately and clearly. No test was executed in this phase.

Expected Root next step is the semantic RED run of:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\windows\native\tests\wix-installer-scenario-tests.ps1
```

The current implementation is expected to aggregate semantic failures rather than emit synthetic or runtime `PASS`. The C++ fixture arithmetic, null-store, and cleanup findings and all runtime/process ancestry issues remain test-first follow-up work; no unsafe fixture execution or scenario runtime is authorized by these test additions.

### Round 6 focused correction

Production-side contracts now implement the accepted Round 6 RED:

- BA authorization requires a nonempty extracted-payload allowlist distinct from the setup hash; setup-hash images are `burn_engine`. Discovery and final stop both verify held process creation times, token elevation, exact identity, and fresh ancestry from the captured root.
- Candidate MSI commits are filtered by `expected_msi_version`, require install (not removal) `InstallFinalize` plus install status 0 or 3010, and use only MSI paths explicitly referenced by the current Burn log. Raw copies receive collision-safe sequence prefixes.
- Burn timestamps with or without a zone use one converter; no-zone values are local-time then UTC. No `IsPathFullyQualified` dependency remains for PowerShell 5.1.
- The state fixture uses `std::chrono::steady_clock`, checks both `DoctorStore::open` calls, and never recursively deletes settings/Doctor state after a partial failure.
- Runtime readback uses the MSI owner registry key and installed manifest version, dynamic `files` count, every hash/size, actual COM server path/hash, registration status, Burn registration, and explicit retention booleans. Runtime versions are never filled from MSI 70.x values.
- A 3010 candidate commit is evidence of restart-required, not corruption; receipts mark a Root-owned guest reboot phase and do not emit an actual scenario `PASS` before that phase.

No test, build, installer, process, registry, network, WSL, or Git operation was executed for this correction. Root independently owns the next native `PS7`/`PS5` contract runs and Neon runtime review.

### Round 7 focused correction

Neon 7's Token-absence finding is rejected. Root's read-only guest `owner-field-check.json` observed `Token` present and nonempty, but did not export its content. The earlier `baseline-readback.json` intentionally projected only six named fields, so that projection's omission was not evidence of absence. The installed identity still requires a nonempty Token through safe property reads, alongside Owner, Version, Sid, ManifestSha256, SafeRetire, and DoctorLifecycle.

Four focused harness corrections were applied:

- MSI parsing now accepts a candidate `Windows Installer installed the product` summary with Product Version and status `0`/`3010`, otherwise uses the last `ProductVersion` property; removal success alone cannot commit, while a same-log inner removal plus outer candidate installation can commit exactly once and retain `restart_required`.
- Recovery stops immediately once a formed state exists. A formed `3010` state is preserved, marked Root-guest-reboot-required, and reports `NOT_COVERED` until the external reboot phase; no reset or further injection occurs.
- Process metadata access catches `Win32Exception` and `InvalidOperationException`; `alive` is computed only inside that protected block and becomes null when liveness is ambiguous. BA-only identity, elevation, ancestry, and `alive -eq $true` restrictions remain unchanged.
- Burn bundle matching now additionally requires `values.Installed -eq 1`; matching entries with zero or missing Installed do not qualify.

No command, test, build, installer, process, registry, network, WSL, or Git operation was executed for this focused correction. Root independently owns the resulting contract runs and Neon/runtime verification; this handoff does not claim GREEN or runtime PASS.

## Root verification after implementation review (2026-11-27)

Neon round 8 concluded `SAFE_TO_RUN_RUNTIME` for the reviewed harness, with UI and data review passing. This exhausted the eight-round limit. Root subsequently found Windows PowerShell 5.1 and runtime-observation defects while exercising the harness in the one owned Sandbox. These changes affect the test harness and its checks, not product MSI transactions, the extracted-BA authorization boundary, or process-termination eligibility. They were not submitted as a ninth Neon review.

| Actual failure | Focused correction and evidence |
| --- | --- |
| Atomic JSON replacement treated the third `File.Replace` argument as an empty path in PowerShell 5.1 | Pass the typed PowerShell null string. A new pure regression failed first; 312 pure assertions then passed on PS5/PS7, and two actual mapped-output replacements left no temporary file. |
| A short-lived native child produced null `ExitCode` / empty-file values | Wait for synchronous child completion and read completed streams as strings. Native exit 0 and 23 assertions failed before the correction and passed afterward. |
| Retention-path array parsing passed an array as a `Join-Path` child | Parenthesize each path expression. The actual pre-install snapshot then proceeded to the independent inventory check. |
| MSI inventory used an unsupported COM property projection and an invalid `ProductInfo` field | Use read-only indexed `RelatedProducts` and `ProductInfo` through COM reflection, releasing both COM objects. The native regression failed first and passed after correction; an actual baseline snapshot verified 12 payloads, manifest, registration, and runtime 0.1.1. No `Win32_Product` query or repair action is used. |
| A DLL path containing spaces reached the mapped-module fixture as several arguments | Quote DLL and ready-file paths, retain the asynchronous process handle, and quote Burn log paths. An actual two-second fixture using a spaced DLL and output directory failed first and then passed. |
| A live Burn log could not be read while the installer held its writer open | Read live logs with `FileShare.ReadWrite` and `FileShare.Delete`. An actual open-writer regression failed before this helper existed, then passed. Reentry now waits for the first candidate MSI execution marker and refuses a second launch after the first process or Apply has completed. |

`wix-installer-native-capture-tests.ps1` is repository test source and is included in the Windows CI workflow beside the pure contract suite. Its latest PS5 and PS7 runs passed native exit-code/empty-output, live-log sharing, read-only MSI enumeration, and bounded spaced-path module checks. These checks start no installer, write no registry value, and terminate no process; they do not establish a product scenario PASS.

### Later control run and focused negative TDD

The early helper/UAC failures above remain failures. After the user resumed, a new sole Sandbox installed the exact official 0.1.1 baseline normally. Original-control trial 6 then reached actual MSI execution and exposed a product defect: the second client reached Apply and received MSI 1618, but its SessionEnd None removed the shared Burn registration before the first MSI committed 3010. The first client's SessionEnd Full subsequently failed with 0x80070002. After the guest restarted, payloads and MSI existed but the Burn uninstall registration was absent. **Actual Reentry is FAIL**, not a native-lock PASS. Source inspection then established that WiX 6.0.2 ApplyLock's mutex code is disabled under `#if 0`.

Two additional MAI test-first corrections have independent RED/GREEN receipts:

| Gap | RED | GREEN and scope |
| --- | --- | --- |
| Passive test setup could automatically restart before Root collected 3010 readback | Native-capture run 12 failed SCN005 because `/norestart` was missing | Add `/norestart` only to passive invocations; ordinary recovery with no options still receives exactly zero arguments. PS5 run 13 and PS7 run 14 passed. The regression AST-loads the real helper with a local Start-Process mock and starts no setup. |
| Reentry could falsely PASS when the first installer had lost Burn registration, despite native 1618 and a first MSI 3010 commit | Pure-contract run 20 reported 33 semantic failures | Reuse the pure Upgrade verdict for the first installer's full final state. False readback flags produce FAIL, missing/null evidence produces NOT_COVERED, and runtime/MSI version mismatches cannot pass. PS5 run 21 and PS7 run 22 each passed 505 assertions. These are synthetic classification checks, not product validation. |

Root also completed a real ordinary zero-argument same-EXE recovery of this concurrent-client incident: Burn registration was restored without executing another MSI; all 12 payloads, COM, owner, versions, settings and disabled Doctor history matched. This is **not** a BA interruption and does not consume or replace the two permitted candidate-BA injections, which remain zero.

The same EXE's normal maintenance UI then uninstalled the control. Root retained the files-in-use applications and selected deferred cleanup. Independent readback before and after a guest-only restart confirmed no related MSI/Burn uninstall entries, COM, owner, product directory or product pending-deletes; settings and Doctor history remained. The native status tool's GetProfile E_FAIL was preserved as expected for an absent profile, not rewritten as an all-zero result. Empty TIP root keys were not required to disappear. With no installation process active, Root closed the sole guest to release idle resources; all mapped evidence remains, but its guest-local state is gone.

### Runtime coverage still required

The product guard is not yet implemented. The minimal official BAFunctions design and its Create-failure-to-skip-cleanup call chain are recorded in [the guard design](installer-reentry-minimal-design.md). Its Win32 lifetime tests are now being written test-first; compile/header feasibility is not a product PASS.

The corrected artifact still needs actual pre-transaction refusal (including initially elevated launch), complete first-transaction readback, guarded related-bundle upgrade/restore, font-correct Progress/Failure/Success/FilesInUse/uninstall screens, registered input, and the bounded actual BA interruption/recovery checks. A cancelled UAC, pure classifier PASS, direct-MSI supplement, or UI preview cannot replace those checks. Issue 16 remains open; the historical 0x800700e8 cause is still unproven. No ninth Neon review, delivery commit, push, PR or release has occurred.
