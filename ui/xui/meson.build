xemu_ss.add(files(
  'actions.cc',
  'animation.cc',
  'compat.cc',
  'debug.cc',
  'font-manager.cc',
  'input-manager.cc',
  'main-menu.cc',
  'main.cc',
  'misc.cc',
  'menubar.cc',
  'monitor.cc',
  'notifications.cc',
  'popup-menu.cc',
  'reporting.cc',
  'scene-components.cc',
  'scene-manager.cc',
  'scene.cc',
  'snapshot-manager.cc',
  'viewport-manager.cc',
  'welcome.cc',
  'widgets.cc',
))

if opengl.found()
  xemu_ss.add(files('gl-helpers.cc'))
else
  # No OpenGL (the PS5): draw with the NV2A Vulkan renderer's device.
  xemu_ss.add(volk, files('vk-helpers.cc', 'vk-present.cc'))
endif

if host_os == 'freebsd' # The PS5's front end
  xemu_ss.add(files('dashboard.cc', 'cover-download.cc', 'game-log.cc', 'game-patches.cc',
                    'game-profile.cc',
                    'ui-sounds.cc',
                    'xiso.cc'))
endif

if host_os == 'windows'
  xemu_ss.add(files('update.cc', 'win32-dxgi-present.cc'))
endif
