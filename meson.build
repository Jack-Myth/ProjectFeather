project(
  'project-feather',
  'cpp',
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
  'src/Vm/Vm.cpp',
  'src/Object/Object.cpp',
  'src/Compiler/Compiler.cpp',
  include_directories : include_directories('include'),
)

FeatherDependency = declare_dependency(
  link_with : FeatherCore,
  include_directories : include_directories('include'),
)

subdir('tests')
