NetworkSources = files('Network.cpp', 'Platform.cpp')
NetworkDependencies = [FeatherDependency]
if host_machine.system() == 'windows'
  NetworkDependencies += meson.get_compiler('cpp').find_library('ws2_32')
endif
NetworkEntrySource = files('NetworkModule.cpp')
