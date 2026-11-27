# The supported product baseline is Windows 10. Do not inherit a toolchain's
# default SDK target: MSYS2 and w64devkit use different defaults.
@('-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DNTDDI_VERSION=0x0A000000')
