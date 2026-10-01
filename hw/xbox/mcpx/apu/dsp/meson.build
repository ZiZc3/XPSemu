# dsp56300 ships prebuilt for Linux, Windows and macOS only. Elsewhere (the PS5)
# the JIT is left out and the C interpreter always runs.
dsp56300_emu_ffi = dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'],
                              required: host_os in ['linux', 'windows', 'darwin'])

subdir('interp')

libdsp_sources = files(['dsp.c', 'dsp_c.c', 'dsp_dma.c'])
libdsp_c_args = []
if dsp56300_emu_ffi.found()
  libdsp_sources += files('dsp_jit.c')
  libdsp_c_args += '-DCONFIG_DSP56300_JIT'
endif

libdsp = static_library(
  'dsp',
  libdsp_sources + genh,
  c_args: libdsp_c_args,
  dependencies: [dsp56300_emu_ffi],
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
  dependencies: [dsp56300_emu_ffi],
)

mcpx_ss.add(dsp, files('gp_ep.c'))
