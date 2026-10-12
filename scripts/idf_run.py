#!/usr/bin/env python
"""idf.py 包装器：在 MSYS/MinGW shell 中运行时移除 MSYSTEM 变量，
以绕过 ESP-IDF 对 MSys/Mingw 环境的检查。
用法: python idf_run.py build / flash / monitor ...
"""
import os
import sys

os.environ.pop('MSYSTEM', None)

IDF_PATH = os.environ.get('IDF_PATH')
if not IDF_PATH:
    sys.exit('IDF_PATH is not set')

sys.path.insert(0, os.path.join(IDF_PATH, 'tools'))

import idf  # noqa: E402

idf.main(sys.argv[1:])