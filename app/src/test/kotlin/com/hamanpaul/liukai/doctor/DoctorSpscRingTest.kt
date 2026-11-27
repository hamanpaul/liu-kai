package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.core.doctor.DoctorInputScope
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class DoctorSpscRingTest {
    @Test
    fun `bounded ring preserves FIFO order without overwriting`() {
        val ring = DoctorSpscRing(2)

        assertTrue(ring.tryWrite { it.operationId = 11L })
        assertTrue(ring.tryWrite { it.operationId = 12L })
        assertFalse(ring.tryWrite { it.operationId = 13L })
        assertEquals(2L, ring.depth)

        val operations = mutableListOf<Long>()
        while (ring.poll { operations += it.operationId }) Unit

        assertEquals(listOf(11L, 12L), operations)
        assertEquals(0L, ring.depth)
    }

    @Test
    fun `discard counts only ordinary records for the caller`() {
        val ring = DoctorSpscRing(2)
        ring.tryWrite {
            it.operationId = 1L
            it.scope = DoctorInputScope.ORDINARY
        }
        ring.tryWrite {
            it.operationId = 0L
            it.scope = DoctorInputScope.UNKNOWN
        }

        var ordinary = 0
        ring.discard {
            if (it.scope == DoctorInputScope.ORDINARY) ordinary += 1
        }

        assertEquals(1, ordinary)
        assertEquals(0L, ring.depth)
    }
}
