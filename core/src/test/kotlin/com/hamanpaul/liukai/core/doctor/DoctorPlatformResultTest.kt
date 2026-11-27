package com.hamanpaul.liukai.core.doctor

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class DoctorPlatformResultTest {
    private val event = DoctorEventRecord(
        producerId = 17, sessionId = 23, epoch = 31, operationId = 7, sequence = 1,
        monotonicNs = 1_000_000, wallUtcMs = 1_764_057_600_000,
        timezoneOffsetMinutes = 480, eventCode = DoctorEventCode.EDIT_REQUEST,
        callbackSource = DoctorCallbackSource.KEY_ACTION, scope = DoctorInputScope.ORDINARY,
        mode = DoctorEventMode.LIU, phase = DoctorEventPhase.COMPOSITION,
        focus = true, handled = false, resultCode = DoctorResultCode.FAILED, durationUs = 1,
        platformResultCode = -2_147_024_891,
    )

    @Test
    fun `平台錯誤碼保留 signed32 數值並符合共享 golden`() {
        val golden = doctorFixture("platform-result.jsonl")
        val encoded = DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, event)
        assertTrue(encoded is DoctorEventEncode.Accepted)
        assertEquals(golden, encoded.line + "\n")
        val decoded = DoctorEventCodec.decodeJsonl(golden)
        assertNull(decoded.failure)
        assertEquals(listOf(event), decoded.events.map { it.event })
    }

    @Test
    fun `平台結果可省略並保留零與 signed32 兩端點`() {
        listOf(null, 0, Int.MIN_VALUE, Int.MAX_VALUE).forEach { code ->
            val value = event.copy(platformResultCode = code)
            val encoded = DoctorEventCodec.encodeLine(DOCTOR_TEST_BUILD, value)
            assertTrue(encoded is DoctorEventEncode.Accepted)
            val decoded = DoctorEventCodec.decodeJsonl(encoded.line + "\n")
            assertNull(decoded.failure)
            assertEquals(value, decoded.events.single().event)
            assertEquals(code != null, encoded.line.contains("\"platformResultCode\":"))
        }
    }

    @Test
    fun `平台結果拒絕超界 字串 布林 小數與巢狀資料`() {
        val line = doctorFixture("platform-result.jsonl")
        listOf("2147483648", "-2147483649", "\"failure\"", "true", "0.5", "{}").forEach { invalid ->
            val polluted = line.replace("\"platformResultCode\":-2147024891", "\"platformResultCode\":$invalid")
            assertEquals(DoctorEventFailure.INVALID_TYPE, DoctorEventCodec.decodeJsonl(polluted).failure)
        }
    }
}
