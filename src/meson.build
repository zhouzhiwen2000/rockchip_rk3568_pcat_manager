pcat_sources = [
    'main.c',
    'pmu-manager.c',
    'modem-manager.c',
    'controller.c'
]

pcat_headers = [
    'common.h',
    'pmu-manager.h',
    'modem-manager.h',
    'controller.h'
]

executable('pcat-manager',
    pcat_sources,
    pcat_headers,
    install: true,
    dependencies : [
        glib2_deps,
        gthread2_deps,
        gio2_deps,
        gio2_unix_deps,
        libusb1_deps,
        jsonc_deps,
        gpiod_deps
    ]
)
