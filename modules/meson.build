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

HelperSource = configure_file(input : '../tests/fixtures/helper.fe',
  output : 'helper.fe', copy : true)
configure_file(input : '../tests/fixtures/stdio-shadow.fe',
  output : 'stdio.fe', copy : true)
HelperBytecode = custom_target('helper-bytecode',
  input : '../tests/fixtures/helper.fe', output : 'helper.fbc',
  command : [FeatherCompilerCli, '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
