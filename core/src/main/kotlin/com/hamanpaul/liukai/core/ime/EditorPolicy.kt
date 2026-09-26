package com.hamanpaul.liukai.core.ime

/** 依 EditorInfo 決定輸入法行為。常數與 android.text.InputType／EditorInfo 相同。 */
object EditorPolicy {
    private const val TYPE_MASK_CLASS = 0x0f
    private const val TYPE_MASK_VARIATION = 0xff0
    private const val TYPE_CLASS_TEXT = 0x1
    private const val TYPE_CLASS_NUMBER = 0x2
    private const val TYPE_TEXT_FLAG_MULTI_LINE = 0x20000
    private val TEXT_PASSWORD_VARIATIONS = setOf(0x80, 0x90, 0xe0)
    private const val TYPE_NUMBER_VARIATION_PASSWORD = 0x10
    private const val IME_MASK_ACTION = 0xff
    private const val IME_ACTION_UNSPECIFIED = 0
    private const val IME_ACTION_NONE = 1
    private const val IME_FLAG_NO_ENTER_ACTION = 0x40000000

    fun isPassword(inputType: Int): Boolean {
        val cls = inputType and TYPE_MASK_CLASS
        val variation = inputType and TYPE_MASK_VARIATION
        return (cls == TYPE_CLASS_TEXT && variation in TEXT_PASSWORD_VARIATIONS) ||
            (cls == TYPE_CLASS_NUMBER && variation == TYPE_NUMBER_VARIATION_PASSWORD)
    }

    fun enterAction(imeOptions: Int, inputType: Int): EnterAction {
        val action = imeOptions and IME_MASK_ACTION
        val triggersAction = imeOptions and IME_FLAG_NO_ENTER_ACTION == 0 &&
            inputType and TYPE_TEXT_FLAG_MULTI_LINE == 0 &&
            action != IME_ACTION_NONE && action != IME_ACTION_UNSPECIFIED
        return if (triggersAction) EnterAction.EditorAction(action) else EnterAction.NewLine
    }
}
