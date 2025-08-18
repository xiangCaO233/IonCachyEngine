-- 设置项目名称
set_project("IonCachyEngin")
set_languages("cxx23")
add_rules("mode.debug", "mode.release")

-- [[ 1. 全局依赖配置 ]]
add_packagedirs("ThirdParty")
add_requires("spdlog", "fmt", "openal-soft", "sdl", "rubberband", "ffmpeg")
set_values("package.link_kind.ffmpeg", "shared")

-- [[ 2. 全局项目配置 ]]
add_includedirs("include")

-- 定义所有库源文件
local lib_sources = { "src/ice/**/*.cpp" }

-- [[ 3. 目标定义 ]]

-- 静态库
target("IonCachyEngin-dev")
set_kind("static")
add_files(lib_sources)
add_includedirs("include", { public = true })
if not is_plat("macosx") then
	if get_config("toolchain") == "msvc" then
		add_cxflags("/arch:AVX2")
	else
		add_cxflags("-mavx2")
	end
end
for _, pkg in ipairs({ "spdlog", "fmt", "openal-soft", "sdl", "rubberband" }) do
	set_values("package.link_kind." .. pkg, "static")
end
add_packages("spdlog", "fmt", "openal-soft", "sdl", "rubberband", "ffmpeg")

-- 动态库
target("libIonCachyEngin")
set_kind("shared")
set_filename("IonCachyEngin")
add_files(lib_sources)
add_includedirs("include", { public = true })
if not is_plat("macosx") then
	if get_config("toolchain") == "msvc" then
		add_cxflags("/arch:AVX2")
	else
		add_cxflags("-mavx2")
	end
end
if is_plat("windows") then
	add_defines("ICE_API_EXPORTS", { public = true })
end
for _, pkg in ipairs({ "spdlog", "fmt", "openal-soft", "sdl", "rubberband" }) do
	set_values("package.link_kind." .. pkg, "shared")
end
add_packages("spdlog", "fmt", "openal-soft", "sdl", "rubberband", "ffmpeg")

-- 可执行文件
target("testIonCachyEngin")
set_kind("binary")
add_files("src/main.cpp")
if not is_plat("macosx") then
	if get_config("toolchain") == "msvc" then
		add_cxflags("/arch:AVX2")
	else
		add_cxflags("-mavx2")
	end
end
add_links("IonCachyEngin")
