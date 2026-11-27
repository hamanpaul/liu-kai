package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.BuildConfig
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class AndroidDoctorBuildTest {
    @Test
    fun `doctor metadata comes from build config and uses a validated fixed commit`() {
        val metadata = AndroidDoctorBuild.metadata

        assertEquals(BuildConfig.VERSION_NAME, metadata.version)
        assertEquals(BuildConfig.DOCTOR_SOURCE_COMMIT, metadata.sourceCommit)
        assertTrue(metadata.version.matches(Regex("[0-9A-Za-z][0-9A-Za-z.+_-]{0,127}")))
        assertTrue(metadata.sourceCommit.matches(Regex("[0-9a-f]{7,40}")))
        assertTrue(
            BuildConfig.DOCTOR_SOURCE_COMMIT_ORIGIN in setOf(
                "development-synthetic",
                "gradle-property",
                "distribution-git-head",
            ),
        )
        if (BuildConfig.DOCTOR_SOURCE_COMMIT_ORIGIN == "development-synthetic") {
            assertEquals("0000000", metadata.sourceCommit)
        }
    }
}
