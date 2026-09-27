package com.hamanpaul.liukai.ime

import android.content.Intent
import android.inputmethodservice.InputMethodService
import android.util.Base64
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Toast
import com.hamanpaul.liukai.core.engine.Language
import com.hamanpaul.liukai.core.engine.LiuEngine
import com.hamanpaul.liukai.core.ime.EditorPolicy
import com.hamanpaul.liukai.core.ime.IcOp
import com.hamanpaul.liukai.core.ime.ImeController
import com.hamanpaul.liukai.core.ime.ImeOutcome
import com.hamanpaul.liukai.core.ime.SoftKey
import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.data.ImePrefs
import com.hamanpaul.liukai.data.TableStore
import com.hamanpaul.liukai.settings.SettingsActivity
import org.json.JSONObject
import java.io.FileDescriptor
import java.io.PrintWriter

/**
 * liu-kai 輸入法服務：只負責把 Android 事件轉給 [ImeController]，
 * 並依序執行它回傳的 InputConnection 操作。輸入邏輯都在 core（純 Kotlin，JVM 單元測試）。
 */
class LiuKaiImeService : InputMethodService(), ImeActions {
    private val controller = ImeController(LiuEngine(null))

    /** 輸入畫面在服務建立時就建好，服務存活期間都存在（不需處理「尚未建立」的狀態）。 */
    private lateinit var view: ImeView

    override fun onCreate() {
        super.onCreate()
        view = ImeView(this, this)
        reloadTables()
        // 還原上次選的語言模式（字表沒有該模式時維持繁中）
        controller.engine.selectLanguage(ImePrefs.language(this))
    }

    private fun reloadTables() {
        val loaded = runCatching { TableStore.load(this) }
            .onFailure { Log.e(TAG, "字表載入失敗", it) }
            .getOrNull()
        controller.tableLoaded = loaded != null
        if (loaded == null) {
            controller.engine.setTables(null, Readings.EMPTY)
        } else {
            controller.engine.setTables(loaded.traditional, loaded.readings, loaded.others)
        }
    }

    /** 框架重建輸入畫面（例如螢幕旋轉）時會再次呼叫：先把 View 從舊的父容器移除再交回。 */
    override fun onCreateInputView(): View {
        (view.parent as ViewGroup?)?.removeView(view)
        return view
    }

    /**
     * 輸入畫面一律顯示（實體鍵盤模式只有候選列，讓候選可以被點選）；鍵盤區是否顯示沿用框架的判斷
     * （沒有實體鍵盤，或使用者開啟「實體鍵盤時也顯示螢幕鍵盤」）。框架在設定變更、螢幕旋轉、
     * 視窗顯示時都會重新呼叫本方法，因此在這裡同步鍵盤區。
     */
    override fun onEvaluateInputViewShown(): Boolean {
        view.setKeyboardVisible(super.onEvaluateInputViewShown())
        return true
    }

    /**
     * 預設實作在有實體鍵盤時會拒絕 App 的隱含顯示請求（點輸入欄），導致候選列不出現；
     * liu-kai 需要候選列，一律接受。
     */
    override fun onShowInputRequested(flags: Int, configChange: Boolean): Boolean = true

    override fun onStartInput(attribute: EditorInfo, restarting: Boolean) {
        super.onStartInput(attribute, restarting)
        reloadTables()
        controller.startInput(attribute.inputType)
    }

    override fun onStartInputView(info: EditorInfo, restarting: Boolean) {
        super.onStartInputView(info, restarting)
        // 換到新的輸入欄：軟鍵盤回到字母層並放開 Shift；Enter 標籤依欄位動作
        view.setEnterAction(EditorPolicy.enterAction(info.imeOptions, info.inputType))
        view.resetLayout()
        view.setKeyboardVisible(super.onEvaluateInputViewShown())
        render()
    }

    /**
     * 預設實作在輸入畫面收起時呼叫 finishComposingText，把組字定案成字母。liu-kai 在畫面收起後仍可用實體鍵盤
     * 繼續組字（打字會重新顯示畫面）；若收起通知晚於接著打的字碼抵達，預設行為會把新的組字定案、與引擎狀態脫節
     * （點候選後欄位變成「ba月」）。因此收起畫面時保留組字，由引擎決定何時上屏或清除。
     */
    override fun onFinishInputView(finishingInput: Boolean) = Unit

    override fun onFinishInput() {
        super.onFinishInput()
        controller.finishInput()
    }

    override fun onUpdateSelection(
        oldSelStart: Int, oldSelEnd: Int, newSelStart: Int, newSelEnd: Int,
        candidatesStart: Int, candidatesEnd: Int,
    ) {
        super.onUpdateSelection(oldSelStart, oldSelEnd, newSelStart, newSelEnd, candidatesStart, candidatesEnd)
        execute(controller.selectionMoved(newSelStart, newSelEnd, candidatesStart, candidatesEnd))
    }

    // ---- 實體鍵盤 ----

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val outcome = controller.keyDown(
            keyCode, event.repeatCount, event.getUnicodeChar(event.metaState),
            event.isCtrlPressed, event.isAltPressed, event.isMetaPressed,
        )
        return handled(outcome, event) || super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        handled(controller.keyUp(keyCode), event) || super.onKeyUp(keyCode, event)

    private fun handled(outcome: ImeOutcome, event: KeyEvent): Boolean {
        execute(outcome.ops, event)
        return outcome.consumed
    }

    // ---- 軟鍵盤與候選列 ----

    override fun onSoftKey(key: SoftKey) {
        val info = currentInputEditorInfo
        execute(controller.softKey(key, info.imeOptions, info.inputType))
    }

    override fun onCandidateTap(index: Int) = execute(controller.tapCandidate(index))

    /** 長按「,」的 ⚙：收起輸入法並開啟 liu-kai 設定頁。 */
    override fun onOpenSettings() {
        requestHideSelf(0)
        startActivity(Intent(this, SettingsActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    /**
     * 語音鍵：與官方相同切換到系統已啟用的語音輸入法（例如 Google 語音輸入），語音輸入結束後由它切回。
     * 沒有啟用任何語音輸入時提示使用者到系統設定啟用。
     */
    override fun onVoice() {
        val imm = getSystemService(InputMethodManager::class.java)
        for (imi in imm.enabledInputMethodList) {
            val voice = imm.getEnabledInputMethodSubtypeList(imi, true).firstOrNull { it.mode == "voice" } ?: continue
            switchInputMethod(imi.id, voice)
            return
        }
        Toast.makeText(this, "請先在系統的螢幕鍵盤設定啟用語音輸入", Toast.LENGTH_SHORT).show()
    }

    /** 長按「同音」選單：切換語言模式並記住。 */
    override fun onLanguage(language: Language) {
        ImePrefs.setLanguage(this, language)
        execute(controller.selectLanguage(language))
    }

    /** 候選列的常用標點：直接上屏（只在沒有組字時顯示）。 */
    override fun onPunctuation(text: String) = execute(listOf(IcOp.Commit(text)))


    // ---- 執行 InputConnection 操作 ----

    /** 依序執行操作；event 為目前的實體鍵事件（只有 ForwardKey 會用到，軟鍵盤與候選列不會產生 ForwardKey）。 */
    private fun execute(ops: List<IcOp>, event: KeyEvent? = null) {
        val ic = currentInputConnection
        ic.beginBatchEdit()
        for (op in ops) {
            when (op) {
                is IcOp.Commit -> ic.commitText(op.text, 1)
                is IcOp.SetComposing -> ic.setComposingText(op.text, 1)
                IcOp.ClearComposing -> {
                    ic.setComposingText("", 1)
                    ic.finishComposingText()
                }
                IcOp.FinishComposing -> ic.finishComposingText()
                is IcOp.SendKey -> sendDownUpKeyEvents(op.keyCode)
                is IcOp.EditorAction -> ic.performEditorAction(op.actionId)
                IcOp.ForwardKey -> ic.sendKeyEvent(event!!)
            }
        }
        ic.endBatchEdit()
        // 實體鍵盤直接打字時輸入畫面可能尚未顯示；開始組字就主動顯示候選列。
        if (controller.needsCandidates && !isInputViewShown) requestShowSelf(0)
        render()
    }

    private fun uiState() = UiState(
        mode = controller.engine.mode,
        // 欄位內顯示的組字（同音鍵前置查詢時帶「'」）
        composing = controller.engine.displayComposing,
        candidates = controller.engine.candidates,
        pageStart = controller.engine.pageStart,
        pageSize = controller.engine.config.pageSize,
        homophoneOf = controller.engine.homophoneOf,
        tableLoaded = controller.tableLoaded,
        failed = controller.engine.failed,
        language = controller.engine.language,
        languages = controller.engine.languages,
        codeHint = controller.engine.codeHint,
    )

    private fun render() {
        view.render(uiState())
    }

    /**
     * 診斷輸出（`adb shell dumpsys activity service <本服務>`）：以 base64 JSON 輸出模式、組字、
     * 候選與按鍵的螢幕座標，供端對端測試與問題回報使用。
     */
    override fun dump(fd: FileDescriptor, fout: PrintWriter, args: Array<out String>) {
        super.dump(fd, fout, args)
        val state = uiState()
        val json = JSONObject()
            .put("mode", state.mode.name)
            .put("tableLoaded", state.tableLoaded)
            .put("composing", state.composing)
            .put("homophoneOf", state.homophoneOf ?: JSONObject.NULL)
            .put("windowShown", isInputViewShown)
            .put("language", controller.engine.language.name)
            .put("languages", org.json.JSONArray(controller.engine.languages.map { it.name }))
            .put("codeHint", controller.engine.codeHint?.let(view::hintText) ?: JSONObject.NULL)
        view.describe(json)
        fout.println("LIUKAI_STATE " + Base64.encodeToString(json.toString().toByteArray(), Base64.NO_WRAP))
    }

    companion object {
        private const val TAG = "LiuKaiIme"
    }
}
