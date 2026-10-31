add_rules("mode.debug", "mode.release")
set_languages("c++23")
set_config("toolchain", "clang-cl[llvm]")
add_cxflags("/utf-8", "/bigobj")
set_runtimes(is_mode("debug") and "MD" or "MT")
set_policy("package.requires_lock", true)
add_requireconfs("vcpkg::*", {configs = {baseline = "1ea949145db9db7c9b254062f94acdaeed947767", shared = false}})
add_requires("vcpkg::opencv4", {configs = {default_features = false}})
add_requires("vcpkg::reflectcpp")

target("CountingMomo")
    set_kind("binary")
    set_default(true)
    add_rules("win.sdk.application")
    set_plat("windows")
    set_arch("x64")
    add_defines("NOMINMAX", "UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0A00")
    add_includedirs("src")
    add_files("src/**.cpp", "resources/app.rc")
    add_packages("vcpkg::opencv4", "vcpkg::reflectcpp")
    add_links("user32", "ole32", "shell32", "d3d11", "d3dcompiler", "dxgi", "dwmapi", "windowsapp",
              "RuntimeObject", "windowscodecs", "d2d1", "dwrite", "dcomp", "gdi32",
              "advapi32", "comdlg32", "uuid")
    if is_mode("release") then
        set_symbols("debug")
        add_ldflags("/DEBUG:FULL", {force = true})
        add_ldflags("/NODEFAULTLIB:libucrt.lib", {force = true})
        add_ldflags("/DEFAULTLIB:ucrt.lib", {force = true})
    end
    on_load(function (target)
        local manifest = io.readfile(path.join(os.projectdir(), "resources/app.manifest"))
        local version = manifest:match('<assemblyIdentity version="([^"]+)"')
        target:add("defines", 'COUNTING_MOMO_VERSION="' .. version .. '"')
        local opencv = target:pkg("vcpkg::opencv4")
        if opencv then
            for _, dir in ipairs(opencv:get("sysincludedirs") or {}) do
                target:add("sysincludedirs", path.join(dir, "opencv4"))
            end
        end
    end)
    after_build(function (target)
        os.cp("resources/scan", path.join(target:targetdir(), "resources/"))
        os.cp("resources/categories", path.join(target:targetdir(), "resources/"))
    end)

target_end()
