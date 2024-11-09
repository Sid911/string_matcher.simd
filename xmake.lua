add_rules("mode.debug", "mode.release", "mode.releasedbg")
add_rules("plugin.compile_commands.autoupdate", { outputdir = "./build" })

set_languages("c++23")
set_toolchains("gcc", "clang")
set_runtimes("stdc++_static")

target("utf8_skip")
  set_kind("static")
  if (is_mode("release", "profile", "releasedbg")) then
    add_vectorexts("avx2")
  end
  add_includedirs("$(projectdir)/include", { public = true })
  add_files("src/utf8_skip.cpp")
target_end()

target("temp")
  set_kind("binary")
  -- if (is_mode("release", "profile", "releasedbg")) then
  --   add_vectorexts("avx2")
  -- end
  add_deps("utf8_skip")
  add_files("src/tmp.cpp")
  add_cxxflags("-march=native", {force = true})
target_end()

target("simd_string")
  set_kind("binary")
  add_deps("utf8_skip")
  add_files("src/simd_string.cpp")
  add_cxxflags("-march=native", {force = true})
target_end()
