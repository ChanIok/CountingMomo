add_rules("mode.debug", "mode.release")
set_languages("c++23")
set_config("toolchain", "clang-cl[llvm]")
add_cxflags("/utf-8", "/bigobj")
set_runtimes(is_mode("debug") and "MD" or "MT")
set_policy("package.requires_lock", true)
add_requireconfs("vcpkg::*", {configs = {baseline = "1ea949145db9db7c9b254062f94acdaeed947767", shared = false}})
add_requires("vcpkg::opencv4", {configs = {default_features = false}})
add_requires("vcpkg::freetype", "vcpkg::reflectcpp")

target("CountingMomo")
    set_kind("binary")
    set_plat("windows")
    set_arch("x64")
    add_defines("NOMINMAX", "UNICODE", "_UNICODE", "WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0A00")
    add_includedirs("src")
    add_files("src/**.cpp", "resources/app.rc")
    add_packages("vcpkg::opencv4", "vcpkg::freetype", "vcpkg::reflectcpp")
    add_links("user32", "ole32", "shell32", "d3d11", "dxgi", "dwmapi", "windowsapp",
              "RuntimeObject", "windowscodecs")
    if is_mode("release") then
        set_symbols("debug")
        add_ldflags("/DEBUG:FULL", {force = true})
        add_ldflags("/NODEFAULTLIB:libucrt.lib", {force = true})
        add_ldflags("/DEFAULTLIB:ucrt.lib", {force = true})
    end
    on_load(function (target)
        local opencv = target:pkg("vcpkg::opencv4")
        if opencv then
            for _, dir in ipairs(opencv:get("sysincludedirs") or {}) do
                target:add("sysincludedirs", path.join(dir, "opencv4"))
            end
        end
    end)
    after_build(function (target)
        os.cp("resources/layout.json", path.join(target:targetdir(), "resources/layout.json"))
        os.cp("resources/clothes_names.txt", path.join(target:targetdir(), "resources/clothes_names.txt"))
        -- 字体、名称库与布局作为显式资源随构建输出。
        if os.isfile("resources/fonts/FZYASHJW_Zhong.ttf") then
            os.cp("resources/fonts/FZYASHJW_Zhong.ttf", path.join(target:targetdir(), "resources/fonts/FZYASHJW_Zhong.ttf"))
        end
    end)

task("package")
    set_menu {usage = "xmake package", description = "Package an existing Release build without rebuilding"}
    on_run(function ()
        import("core.project.project")
        import("core.project.config")
        config.load()
        if config.get("mode") ~= "release" then raise("请先配置 Release 并构建。") end
        local target = project.target("CountingMomo")
        local exe = target:targetfile()
        local font = "resources/fonts/FZYASHJW_Zhong.ttf"
        if not os.isfile(exe) then raise("请先构建 Release。") end
        if not os.isfile(font) then raise("缺少 " .. font .. "；请准备可分发的字体资源。") end
        if not os.isfile("resources/clothes_names.txt") then raise("缺少 resources/clothes_names.txt。") end
        local dest = path.join("dist", "CountingMomo-" .. os.date("%Y%m%d-%H%M%S"))
        os.mkdir(dest)
        os.cp(exe, dest)
        os.cp("resources/layout.json", path.join(dest, "resources/layout.json"))
        os.cp("resources/clothes_names.txt", path.join(dest, "resources/clothes_names.txt"))
        os.cp(font, path.join(dest, "resources/fonts/FZYASHJW_Zhong.ttf"))
        os.cp("README.md", dest)
        print("已打包：" .. dest)
    end)
