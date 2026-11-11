BgfxOptions = CMake.subproject_options()
BgfxOptions.add_cmake_defines(PlatformCMakeDefines + {
  'BGFX_BUILD_EXAMPLES': false,
  'BGFX_BUILD_TESTS': false,
  'BGFX_BUILD_TOOLS': false,
  'BGFX_INSTALL': false,
  'BGFX_CUSTOM_TARGETS': false,
  'BGFX_AMALGAMATED': true,
  'BGFX_CONFIG_RENDERER_WEBGPU': false,
})
if meson.get_compiler('cpp').get_id() == 'msvc'
  BgfxOptions.add_cmake_defines({
    'CMAKE_PROJECT_bgfx_INCLUDE': meson.current_source_dir() / 'bgfx-msvc.cmake',
  })
  # bx supplies /EHs-c- itself. Other C++ targets use Meson's exception setting.
  BgfxOptions.set_override_option('cpp_eh', 'none', target : 'bx')
endif
BgfxProject = CMake.subproject('bgfx-cmake',
  required : Render2DOption.enabled(), options : BgfxOptions)

Render2DModuleAvailable = SdlProject.found() and BgfxProject.found()
if Render2DModuleAvailable
  BgfxDependency = BgfxProject.dependency('bgfx')
  Render2DModuleIncludes = include_directories(
    '.',
    '../../subprojects/bgfx-cmake/bgfx/3rdparty',
    '../../subprojects/bgfx-cmake/bimg/3rdparty')
  Render2DModuleCppArgs = host_machine.system() == 'windows' ? ['/Zc:preprocessor'] : []
  Render2DModuleSources = files('Render2D.cpp', 'Render2DBgfx.cpp', 'Render2DModule.cpp')
  Render2DModuleDependencies = [FeatherDependency, Sdl3Dependency, BgfxDependency]
endif
