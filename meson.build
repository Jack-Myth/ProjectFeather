project(
  'project-feather',
  'cpp',
  meson_version : '>=1.11.0',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

if host_machine.system() == 'windows'
  add_project_arguments('/wd4251', language : 'cpp')
endif

FeatherCore = shared_library(
  'feather-core',
  'src/Value/Value.cpp',
  'src/Bytecode/Bytecode.cpp',
  'src/Bytecode/ProgramImage.cpp',
  'src/Bytecode/SymbolImage.cpp',
  'src/Vm/Vm.cpp',
  'src/Object/Object.cpp',
  'src/Compiler/Compiler.cpp',
  'src/Embed/Import.cpp',
  'src/Snapshot/Snapshot.cpp',
  include_directories : include_directories('include'),
  cpp_args : ['-DFEATHER_CORE_SHARED', '-DFEATHER_CORE_BUILD'],
  install : true,
)

FeatherDependency = declare_dependency(
  link_with : FeatherCore,
  include_directories : include_directories('include'),
  compile_args : ['-DFEATHER_CORE_SHARED'],
)
install_subdir('include/Feather', install_dir : get_option('includedir'))

NativeLoaderSystemDependency = meson.get_compiler('cpp').find_library('dl', required : false)

FeatherCli = executable('feather', 'src/Cli/Main.cpp', 'src/Cli/File.cpp',
  'src/Cli/Runner.cpp', 'src/Cli/ModuleLoader.cpp', 'src/Cli/NativeModules.cpp',
  dependencies : [FeatherDependency, NativeLoaderSystemDependency], install : true)
FeatherCompilerCli = executable('featherc', 'src/Cli/CompilerMain.cpp',
  'src/Cli/File.cpp', dependencies : FeatherDependency, install : true)
FeatherVmCli = executable('feathervm', 'src/Cli/VmMain.cpp',
  'src/Cli/File.cpp', 'src/Cli/Runner.cpp', 'src/Cli/ModuleLoader.cpp',
  'src/Cli/NativeModules.cpp',
  dependencies : [FeatherDependency, NativeLoaderSystemDependency], install : true)

subdir('modules')
subdir('tests')
