package com.hamanpaul.liukai.ime

import com.hamanpaul.liukai.doctor.AndroidDoctorBuildTest
import com.hamanpaul.liukai.doctor.DoctorInputProducerAdditionalTest
import com.hamanpaul.liukai.doctor.DoctorInputProducerTest
import com.hamanpaul.liukai.doctor.DoctorInstrumentationTest
import com.hamanpaul.liukai.doctor.DoctorSettingsPresentationAdditionalTest
import com.hamanpaul.liukai.doctor.DoctorSettingsPresentationTest
import com.hamanpaul.liukai.doctor.DoctorSpscRingTest
import com.hamanpaul.liukai.doctor.DoctorWriterCoreTest
import org.junit.runner.JUnitCore

/** Reuses the JVM regressions on the device; no test logic is copied. */
internal object DoctorUnitDeviceProbe {
    fun run(): String {
        val result = JUnitCore.runClasses(
            AndroidDoctorBuildTest::class.java,
            DoctorInputProducerTest::class.java,
            DoctorInputProducerAdditionalTest::class.java,
            DoctorInstrumentationTest::class.java,
            DoctorSettingsPresentationTest::class.java,
            DoctorSettingsPresentationAdditionalTest::class.java,
            DoctorSpscRingTest::class.java,
            DoctorWriterCoreTest::class.java,
        )
        check(result.runCount > 0 && result.ignoreCount == 0 && result.wasSuccessful()) {
            "Doctor device regressions failed: " + result.failures.joinToString("\n")
        }
        return "Doctor device regressions verified: ${result.runCount} tests."
    }
}
