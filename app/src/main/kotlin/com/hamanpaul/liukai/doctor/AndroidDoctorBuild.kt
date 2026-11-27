package com.hamanpaul.liukai.doctor

import com.hamanpaul.liukai.BuildConfig
import com.hamanpaul.liukai.core.doctor.DoctorBuildMetadata

internal object AndroidDoctorBuild {
    val metadata = DoctorBuildMetadata(
        version = BuildConfig.VERSION_NAME,
        sourceCommit = BuildConfig.DOCTOR_SOURCE_COMMIT,
    )
}
