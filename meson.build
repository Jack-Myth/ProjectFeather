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

FeatherCore = static_library(
  'feather',
  'src/Value/Value.cpp',
  'src/Bytecode/Bytecode.cpp',
  'src/Bytecode/ProgramImage.cpp',
  'src/Vm/Vm.cpp',
  'src/Object/Object.cpp',
  'src/Compiler/Compiler.cpp',
  'src/Embed/Import.cpp',
  'src/Snapshot/Snapshot.cpp',
  include_directories : include_directories('include'),
)

FeatherDependency = declare_dependency(
  link_with : FeatherCore,
  include_directories : include_directories('include'),
)

FeatherStdIo = static_library('feather-stdio', 'stdlib/StdIo.cpp',
  dependencies : FeatherDependency)
FeatherStdIoDependency = declare_dependency(
  link_with : FeatherStdIo,
  dependencies : FeatherDependency,
  include_directories : include_directories('include'),
)

FeatherCli = executable('feather', 'src/Cli/Main.cpp', 'src/Cli/File.cpp',
  'src/Cli/Runner.cpp',
  dependencies : FeatherStdIoDependency, install : true)
FeatherCompilerCli = executable('featherc', 'src/Cli/CompilerMain.cpp',
  'src/Cli/File.cpp', dependencies : FeatherDependency, install : true)
FeatherVmCli = executable('feathervm', 'src/Cli/VmMain.cpp',
  'src/Cli/File.cpp', 'src/Cli/Runner.cpp',
  dependencies : FeatherStdIoDependency, install : true)

subdir('tests')
