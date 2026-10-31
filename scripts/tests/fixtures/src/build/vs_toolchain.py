import os

MSVS_VERSIONS = {'2026': '18.0', '2022': '17.0'}

def GetVisualStudioVersion():
    supported_versions = list(MSVS_VERSIONS.keys())
    return supported_versions[0]
