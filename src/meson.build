pcat_sources = [
    'main.c',
    'pmu-manager.c',
    'modem-manager.c'
]

pcat_headers = [
    'common.h',
    'pmu-manager.h',
    'modem-manager.h'
]

executable('pcat-manager',
    pcat_sources,
    pcat_headers,
    install: true,
    dependencies : [glib2_deps, gthread2_deps, libusb_deps, jsonc_deps, gpiod_deps]
)
