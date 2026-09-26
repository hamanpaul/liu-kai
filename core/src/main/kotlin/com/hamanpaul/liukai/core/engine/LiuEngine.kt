package com.hamanpaul.liukai.core.engine

import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable

enum class InputMode { CHINESE, ENGLISH }

/** 候選項；annotation 為附註（萬用字元顯示字碼、同音顯示注音）。 */
data class Candidate(val text: String, val annotation: String? = null)

sealed interface EngineEvent {
    data object ToggleEnglish : EngineEvent
}

/** 輸入事件（切換模式以外的事件）；只在中文模式且有字表時由引擎處理。 */
sealed interface ImeEvent : EngineEvent {
    /** 可見字元鍵（實體鍵盤或軟鍵盤）。 */
    data class Key(val char: Char) : ImeEvent
    data object Space : ImeEvent
    data object Backspace : ImeEvent
    data object Enter : ImeEvent
    data object Escape : ImeEvent
    data object PageDown : ImeEvent
    data object PageUp : ImeEvent
    /** 觸控點選候選（絕對索引）。 */
    data class Select(val index: Int) : ImeEvent
}

/** 處理結果：commit 為要上屏的文字；consumed=false 表示這個按鍵要交給 App 處理。 */
data class EngineResult(val consumed: Boolean, val commit: String? = null) {
    companion object {
        val CONSUMED = EngineResult(true)
        val PASS = EngineResult(false)
    }
}

data class EngineConfig(
    val pageSize: Int = 10,
    val wildcardLimit: Int = 200,
    val vrsf: Map<Char, Int> = mapOf('v' to 1, 'r' to 2, 's' to 3, 'f' to 4),
    /** 萬用字元：比對零到多個字根。 */
    val wildcard: Char = '*',
    val homophoneKey: Char = '`',
    val pageDownKeys: Set<Char> = setOf('='),
    val pageUpKeys: Set<Char> = setOf('-'),
)

/**
 * 嘸蝦米輸入引擎（純狀態機，不依賴 Android）。行為規格見 docs/plan.md 第 6 節。
 */
class LiuEngine(
    private var table: CompiledTable?,
    private var readings: Readings = Readings.EMPTY,
    val config: EngineConfig = EngineConfig(),
) {
    var mode: InputMode = InputMode.CHINESE
        private set
    var composing: String = ""
        private set
    var candidates: List<Candidate> = emptyList()
        private set
    var pageStart: Int = 0
        private set
    /** 同音模式下被查詢的字；null 表示一般模式。 */
    var homophoneOf: String? = null
        private set
    /** 上一個事件是組字失敗（組字已清除、不出字，畫面以紅框提示）；下一個事件或重置時清除。 */
    var failed: Boolean = false
        private set

    /** 同音模式下組字仍保留原字碼，因此只看組字是否為空。 */
    val isComposing: Boolean get() = composing.isNotEmpty()

    /** table 為繁中字表（匯入時已併入日文區段的假名字碼）。 */
    fun setTables(table: CompiledTable?, readings: Readings) {
        this.table = table
        this.readings = readings
        reset()
    }

    fun reset() {
        composing = ""
        candidates = emptyList()
        pageStart = 0
        homophoneOf = null
        failed = false
    }

    fun handle(event: EngineEvent): EngineResult {
        failed = false
        return when (event) {
            EngineEvent.ToggleEnglish -> toggleEnglish()
            is ImeEvent -> {
                val table = table
                if (mode == InputMode.ENGLISH || table == null) EngineResult.PASS else handleIme(event, table)
            }
        }
    }

    private fun toggleEnglish(): EngineResult {
        reset()
        mode = if (mode == InputMode.ENGLISH) InputMode.CHINESE else InputMode.ENGLISH
        return EngineResult.CONSUMED
    }

    private fun handleIme(event: ImeEvent, table: CompiledTable): EngineResult = when (event) {
        is ImeEvent.Key -> onKey(event.char, table)
        ImeEvent.Space -> onSpace()
        ImeEvent.Backspace -> onBackspace(table)
        ImeEvent.Enter -> onEnter()
        ImeEvent.Escape -> if (isComposing) { reset(); EngineResult.CONSUMED } else EngineResult.PASS
        ImeEvent.PageDown -> page(+1)
        ImeEvent.PageUp -> page(-1)
        is ImeEvent.Select -> select(event.index)
    }

    private fun isAsciiDigit(c: Char) = c in '0'..'9'

    private fun onKey(c: Char, table: CompiledTable): EngineResult {
        val lower = c.lowercaseChar()

        // Shift＋字母：沒有組字時直接輸出大寫英文；組字中為組字失敗。
        if (c.isLetter() && c.isUpperCase()) {
            return if (isComposing) fail() else EngineResult.PASS
        }

        if (homophoneOf != null) {
            return when {
                isAsciiDigit(c) -> selectDigit(c)
                c in config.pageDownKeys -> page(+1)
                c in config.pageUpKeys -> page(-1)
                else -> fail()
            }
        }

        if (composing.isNotEmpty()) {
            if (isAsciiDigit(c)) return selectDigit(c)
            if (c == config.homophoneKey) return homophone(pageStart, table)
            val vrsfIndex = config.vrsf[lower]
            // 含萬用字元的組字不可能是合法字碼，isCode 已排除，不必另外判斷
            if (vrsfIndex != null) {
                val extended = composing + lower
                if (!table.hasPrefix(extended) && table.isCode(composing) && vrsfIndex < candidates.size) {
                    return commitAt(vrsfIndex)
                }
            }
            if (lower !in table.alphabet && c != config.wildcard) {
                if (c in config.pageDownKeys) return page(+1)
                if (c in config.pageUpKeys) return page(-1)
                return fail()
            }
        } else if (lower !in table.alphabet && c != config.wildcard) {
            return EngineResult.PASS
        }

        if (!hasWildcard() && c != config.wildcard && composing.length >= table.maxCodeLength) {
            return EngineResult.CONSUMED
        }
        composing += if (c == config.wildcard) c else lower
        refreshCandidates(table)
        return EngineResult.CONSUMED
    }

    private fun hasWildcard() = config.wildcard in composing

    private fun onSpace(): EngineResult {
        if (!isComposing) return EngineResult.PASS
        if (candidates.isEmpty()) {
            // 空碼：清除組字，直接出空白
            reset()
            return EngineResult(true, " ")
        }
        return commitAt(pageStart)
    }

    private fun onBackspace(table: CompiledTable): EngineResult {
        if (homophoneOf != null) {
            homophoneOf = null
            refreshCandidates(table)
            return EngineResult.CONSUMED
        }
        if (composing.isEmpty()) return EngineResult.PASS
        composing = composing.dropLast(1)
        refreshCandidates(table)
        return EngineResult.CONSUMED
    }

    private fun onEnter(): EngineResult {
        if (!isComposing) return EngineResult.PASS
        val raw = composing
        reset()
        return EngineResult(true, raw)
    }

    private fun page(direction: Int): EngineResult {
        if (!isComposing) return EngineResult.PASS
        val next = pageStart + direction * config.pageSize
        if (next in candidates.indices) pageStart = next
        return EngineResult.CONSUMED
    }

    /** 數字鍵 0–9 選目前頁第 1–10 個候選：0 為預設字（即空白上屏的字），1–9 依序為其後候選。 */
    private fun selectDigit(c: Char): EngineResult {
        val offset = c - '0'
        val index = pageStart + offset
        return if (offset < config.pageSize && index < candidates.size) commitAt(index) else EngineResult.CONSUMED
    }

    private fun select(index: Int): EngineResult =
        if (index in candidates.indices) commitAt(index) else EngineResult.CONSUMED

    private fun commitAt(index: Int): EngineResult {
        val text = candidates[index].text
        reset()
        return EngineResult(true, text)
    }

    /** 組字失敗：清除組字、不出字，按鍵也不交給 App；畫面依 failed 提示。 */
    private fun fail(): EngineResult {
        reset()
        failed = true
        return EngineResult.CONSUMED
    }

    private fun homophone(index: Int, table: CompiledTable): EngineResult {
        val target = candidates.getOrNull(index)?.text ?: return EngineResult.CONSUMED
        val ch = target.substring(0, target.offsetByCodePoints(0, 1))
        val own = readings.of(ch)
        val list = ArrayList<Candidate>()
        list += Candidate(ch, own.joinToString("／").ifEmpty { "無讀音資料" })
        for (h in readings.homophones(ch, table)) {
            if (h != ch) list += Candidate(h, readings.of(h).firstOrNull())
        }
        homophoneOf = ch
        candidates = list
        pageStart = 0
        return EngineResult.CONSUMED
    }

    private fun refreshCandidates(table: CompiledTable) {
        pageStart = 0
        if (composing.isEmpty()) {
            candidates = emptyList()
            return
        }
        candidates = if (hasWildcard()) {
            table.wildcard(composing, config.wildcardLimit, config.wildcard)
                .map { (text, code) -> Candidate(text, code) }
        } else {
            // 假名字碼（羅馬拼音＋, 為平假名、＋. 為片假名）已於匯入時併入，候選順序照字表。
            table.candidates(composing).map { Candidate(it) }
        }
    }
}
