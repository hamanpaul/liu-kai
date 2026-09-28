package com.hamanpaul.liukai.core.engine

import com.hamanpaul.liukai.core.reading.Readings
import com.hamanpaul.liukai.core.table.CompiledTable

enum class InputMode { CHINESE, ENGLISH, JAPANESE }

/** 候選項；annotation 為附註（萬用字元顯示字碼、同音顯示注音、片假名標示）。 */
data class Candidate(val text: String, val annotation: String? = null)

sealed interface EngineEvent {
    /** 可見字元鍵（實體鍵盤或軟鍵盤）。 */
    data class Key(val char: Char) : EngineEvent
    data object Space : EngineEvent
    data object Backspace : EngineEvent
    data object Enter : EngineEvent
    data object Escape : EngineEvent
    data object PageDown : EngineEvent
    data object PageUp : EngineEvent
    /** 觸控點選候選（絕對索引）。 */
    data class Select(val index: Int) : EngineEvent
    /** 同音字／讀音查詢；index 為 null 時查目前頁首選。 */
    data class Homophone(val index: Int?) : EngineEvent
    data object ToggleEnglish : EngineEvent
    data object ToggleJapanese : EngineEvent
}

/**
 * 處理結果：commit 為要上屏的文字；consumed=false 表示這個按鍵還要交給 App 處理
 * （commit 與 consumed=false 可同時成立：先上屏首選，再讓 App 收到按鍵）。
 */
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
    val wildcardOne: Char = '?',
    val wildcardMany: Char = '*',
    val homophoneKey: Char = '`',
    val pageDownKeys: Set<Char> = setOf('='),
    val pageUpKeys: Set<Char> = setOf('-'),
)

/**
 * 嘸蝦米輸入引擎（純狀態機，不依賴 Android）。行為規格見 docs/plan.md 第 6 節。
 */
class LiuEngine(
    private var traditional: CompiledTable?,
    private var japanese: CompiledTable? = null,
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

    private var modeBeforeEnglish: InputMode = InputMode.CHINESE

    val isComposing: Boolean get() = composing.isNotEmpty() || homophoneOf != null

    fun setTables(traditional: CompiledTable?, japanese: CompiledTable?, readings: Readings) {
        this.traditional = traditional
        this.japanese = japanese
        this.readings = readings
        reset()
    }

    fun reset() {
        composing = ""
        candidates = emptyList()
        pageStart = 0
        homophoneOf = null
    }

    private val activeTable: CompiledTable?
        get() = when (mode) {
            InputMode.CHINESE -> traditional
            InputMode.JAPANESE -> japanese
            InputMode.ENGLISH -> null
        }

    fun handle(event: EngineEvent): EngineResult = when (event) {
        EngineEvent.ToggleEnglish -> toggleEnglish()
        EngineEvent.ToggleJapanese -> toggleJapanese()
        else -> if (mode == InputMode.ENGLISH || activeTable == null) EngineResult.PASS else handleIme(event)
    }

    private fun toggleEnglish(): EngineResult {
        reset()
        if (mode == InputMode.ENGLISH) {
            mode = modeBeforeEnglish
        } else {
            modeBeforeEnglish = mode
            mode = InputMode.ENGLISH
        }
        return EngineResult.CONSUMED
    }

    private fun toggleJapanese(): EngineResult {
        if (japanese == null) return EngineResult.CONSUMED
        reset()
        mode = if (mode == InputMode.JAPANESE) InputMode.CHINESE else InputMode.JAPANESE
        return EngineResult.CONSUMED
    }

    private fun handleIme(event: EngineEvent): EngineResult = when (event) {
        is EngineEvent.Key -> onKey(event.char)
        EngineEvent.Space -> onSpace()
        EngineEvent.Backspace -> onBackspace()
        EngineEvent.Enter -> onEnter()
        EngineEvent.Escape -> if (isComposing) { reset(); EngineResult.CONSUMED } else EngineResult.PASS
        EngineEvent.PageDown -> page(+1)
        EngineEvent.PageUp -> page(-1)
        is EngineEvent.Select -> select(event.index)
        is EngineEvent.Homophone -> homophone(event.index ?: pageStart)
        EngineEvent.ToggleEnglish, EngineEvent.ToggleJapanese -> EngineResult.CONSUMED
    }

    private fun onKey(c: Char): EngineResult {
        val table = activeTable ?: return EngineResult.PASS
        val lower = c.lowercaseChar()

        // Shift＋字母直接輸出大寫英文，不當字根。
        if (c.isLetter() && c.isUpperCase()) {
            return if (isComposing) commitFirstThenPass() else EngineResult.PASS
        }

        if (homophoneOf != null) {
            return when {
                c.isDigit() -> selectDigit(c)
                c in config.pageDownKeys -> page(+1)
                c in config.pageUpKeys -> page(-1)
                else -> commitFirstThenPass()
            }
        }

        if (composing.isNotEmpty()) {
            if (c.isDigit()) return selectDigit(c)
            if (c == config.homophoneKey) return homophone(pageStart)
            val vrsfIndex = config.vrsf[lower]
            if (vrsfIndex != null && !hasWildcard()) {
                val extended = composing + lower
                if (!table.hasPrefix(extended) && table.isCode(composing) && vrsfIndex < candidates.size) {
                    return commitAt(vrsfIndex)
                }
            }
            if (lower !in table.alphabet && c !in wildcardChars()) {
                if (c in config.pageDownKeys) return page(+1)
                if (c in config.pageUpKeys) return page(-1)
                return commitFirstThenPass()
            }
        } else if (lower !in table.alphabet && c !in wildcardChars()) {
            return EngineResult.PASS
        }

        if (!hasWildcard() && c !in wildcardChars() && composing.length >= table.maxCodeLength) {
            return EngineResult.CONSUMED
        }
        composing += if (c in wildcardChars()) c else lower
        refreshCandidates()
        return EngineResult.CONSUMED
    }

    private fun wildcardChars() = setOf(config.wildcardOne, config.wildcardMany)

    private fun hasWildcard() = composing.any { it in wildcardChars() }

    private fun onSpace(): EngineResult {
        if (!isComposing) return EngineResult.PASS
        if (candidates.isEmpty()) return EngineResult.CONSUMED
        return commitAt(pageStart)
    }

    private fun onBackspace(): EngineResult {
        if (homophoneOf != null) {
            homophoneOf = null
            refreshCandidates()
            return EngineResult.CONSUMED
        }
        if (composing.isEmpty()) return EngineResult.PASS
        composing = composing.dropLast(1)
        refreshCandidates()
        return EngineResult.CONSUMED
    }

    private fun onEnter(): EngineResult {
        if (!isComposing) return EngineResult.PASS
        val raw = composing
        reset()
        return if (raw.isEmpty()) EngineResult.CONSUMED else EngineResult(true, raw)
    }

    private fun page(direction: Int): EngineResult {
        if (!isComposing) return EngineResult.PASS
        val next = pageStart + direction * config.pageSize
        if (next in candidates.indices) pageStart = next
        return EngineResult.CONSUMED
    }

    private fun selectDigit(c: Char): EngineResult {
        val offset = if (c == '0') 9 else c - '1'
        return if (offset < config.pageSize && pageStart + offset in candidates.indices) commitAt(pageStart + offset) else EngineResult.CONSUMED
    }

    private fun select(index: Int): EngineResult =
        if (index in candidates.indices) commitAt(index) else EngineResult.CONSUMED

    private fun commitAt(index: Int): EngineResult {
        val text = candidates[index].text
        reset()
        return EngineResult(true, text)
    }

    private fun commitFirstThenPass(): EngineResult {
        val first = candidates.getOrNull(pageStart)?.text
        reset()
        return EngineResult(false, first)
    }

    private fun homophone(index: Int): EngineResult {
        val table = traditional ?: return EngineResult.CONSUMED
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

    private fun refreshCandidates() {
        pageStart = 0
        val table = activeTable
        if (table == null || composing.isEmpty()) {
            candidates = emptyList()
            return
        }
        candidates = if (hasWildcard()) {
            table.wildcard(composing, config.wildcardLimit, config.wildcardOne, config.wildcardMany)
                .map { (text, code) -> Candidate(text, code) }
        } else {
            // 日文段本身以「羅馬拼音＋,」輸出平假名、「＋.」輸出片假名，候選順序照字表，不另外插入變體。
            table.candidates(composing).map { Candidate(it) }
        }
    }
}
