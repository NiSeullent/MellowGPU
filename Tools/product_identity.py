"""Active MellowGPU packaging identity; legacy bundles require replacement, never co-loading."""
PRODUCT = 'MellowGPU'
BUNDLE = PRODUCT + '.kext'
BUNDLE_ID = 'com.NiSeullent.MellowGPU'
EXECUTABLE = PRODUCT
VERSION = '0.5.1'
DEVELOPMENT_LABEL = VERSION + '-dev'
LEGACY_BUNDLE = 'Mellow.kext'
LEGACY_BUNDLE_ID = 'com.NiSeullent.Mellow'


def validate_product_info(info):
    expected = {'CFBundleIdentifier': BUNDLE_ID, 'CFBundleExecutable': EXECUTABLE,
                'CFBundleName': PRODUCT, 'CFBundleVersion': VERSION,
                'CFBundleShortVersionString': VERSION,
                'MellowGPUDevelopmentLabel': DEVELOPMENT_LABEL}
    for key, value in expected.items():
        if info.get(key) != value:
            raise ValueError('MellowGPU product metadata mismatch: ' + key)


def validate_configured_product(items):
    """Validate all (OpenCore BundlePath, Info.plist) pairs, including disabled entries.

    Looking at metadata catches a legacy bundle renamed on disk. Exactly one
    current bundle is required; a disabled baseline still has that one entry.
    """
    current = 0
    for path, info in items:
        identifier = info.get('CFBundleIdentifier')
        if path == LEGACY_BUNDLE or identifier == LEGACY_BUNDLE_ID:
            raise ValueError('Replace legacy Mellow before configuring MellowGPU')
        if path == BUNDLE or identifier == BUNDLE_ID:
            current += 1
            if path != BUNDLE or identifier != BUNDLE_ID or info.get('CFBundleExecutable') != EXECUTABLE or info.get('CFBundleVersion') != VERSION:
                raise ValueError('MellowGPU bundle path, executable, identifier or version mismatch')
            validate_product_info(info)
    if current != 1:
        raise ValueError('Expected exactly one current MellowGPU product')
