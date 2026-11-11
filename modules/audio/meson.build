OggOptions = CMake.subproject_options()
OggOptions.add_cmake_defines(PlatformCMakeDefines + {'BUILD_SHARED_LIBS': false, 'BUILD_TESTING': false})
OggProject = CMake.subproject('ogg', options : OggOptions)
meson.override_dependency('ogg', OggProject.dependency('ogg'))

OpusOptions = CMake.subproject_options()
OpusOptions.add_cmake_defines(PlatformCMakeDefines + {
  'CMAKE_POLICY_VERSION_MINIMUM': '3.5',
  'OPUS_PACKAGE_VERSION': '1.4',
  'OPUS_BUILD_SHARED_LIBRARY': false,
  'OPUS_BUILD_TESTING': false,
  'OPUS_BUILD_PROGRAMS': false,
  'BUILD_TESTING': false,
})
Opus = CMake.subproject('opus', options : OpusOptions)
meson.override_dependency('opus', Opus.dependency('opus'))
OpusFile = subproject('opusfile').get_variable('opusfile_dep')

AudioSource = files('Audio.cpp')
install_data('THIRD-PARTY.txt', rename : 'audio-licenses.txt',
  install_dir : get_option('bindir') / 'modules')
