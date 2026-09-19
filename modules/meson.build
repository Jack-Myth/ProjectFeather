if host_machine.system() == 'windows'
  NativeModuleSuffix = 'dll'
elif host_machine.system() == 'darwin'
  NativeModuleSuffix = 'dylib'
else
  NativeModuleSuffix = 'so'
endif

StdioNativeModule = shared_module('stdio.felib', '../stdlib/StdIo.cpp',
  '../stdlib/StdIoModule.cpp',
  name_prefix : '',
  name_suffix : NativeModuleSuffix,
  dependencies : FeatherDependency,
  install : true,
  install_dir : get_option('bindir') / 'modules')

SnapshotNativeModule = shared_module('snapshot.felib', '../stdlib/Snapshot.cpp',
  '../stdlib/SnapshotModule.cpp',
  name_prefix : '',
  name_suffix : NativeModuleSuffix,
  dependencies : FeatherDependency,
  install : true,
  install_dir : get_option('bindir') / 'modules')

subdir('time')
TimeNativeModule = shared_module('time.felib', TimeModuleSources,
  name_prefix : '',
  name_suffix : NativeModuleSuffix,
  dependencies : FeatherDependency,
  install : true,
  install_dir : get_option('bindir') / 'modules')

Render2DOption = get_option('render2d')
if not Render2DOption.disabled()
  subdir('render2d')
  if Render2DModuleAvailable
    Render2DNativeModule = shared_module('render2d.felib',
      Render2DModuleSources,
      name_prefix : '',
      name_suffix : NativeModuleSuffix,
      include_directories : Render2DModuleIncludes,
      cpp_args : Render2DModuleCppArgs,
      dependencies : Render2DModuleDependencies,
      install : true,
      install_dir : get_option('bindir') / 'modules')
  endif
endif

HelperSource = configure_file(input : '../tests/fixtures/helper.fe',
  output : 'helper.fe', copy : true)
configure_file(input : '../tests/fixtures/stdio-shadow.fe',
  output : 'stdio.fe', copy : true)
HelperBytecode = custom_target('helper-bytecode',
  input : '../tests/fixtures/helper.fe', output : 'helper.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
