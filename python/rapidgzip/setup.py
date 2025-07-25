#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import copy
import os
import platform
import shutil
import sys
import tempfile
from distutils.errors import CompileError

from setuptools import setup
from setuptools.extension import Extension
from setuptools.command.build_ext import build_ext

# For nasm_extension, which is in the same folder as this setup.py during the build process.
sys.path.append(os.path.dirname(__file__))

# This fallback is only for jinja, which is used by conda to analyze this setup.py before any build environment
# is set up.
try:
    from Cython.Build import cythonize
except ImportError:
    cythonize = None

# Valid options for dependencies:
#   RAPIDGZIP_BUILD_CXXOPT
#   RAPIDGZIP_BUILD_ISAL
#   RAPIDGZIP_BUILD_RPMALLOC
#   RAPIDGZIP_BUILD_ZLIB
# Valid values for dependencies:
#   enable
#   disable
#   system
# Not specifying and option, implies 'enable', which will use the packaged source code for each dependency.
# cxxopts can not be disabled!
optionsPrefix = 'RAPIDGZIP_BUILD_'
buildConfig = {key: value for key, value in os.environ.items() if key.startswith(optionsPrefix)}
print("\nRapidgzip build options:")
for key, value in buildConfig.items():
    print(f"  {key}={value}")


def getDependencyOption(key):
    key = optionsPrefix + key
    option = buildConfig.get(key, 'enable')
    validValues = ['enable', 'disable', 'system']
    if option not in validValues:
        print(f"Unrecognized option for rapidgzip dependency: {key}={option}. Valid values are: {validValues}")
    return option if option == 'system' or option == 'enable' else 'disable'


withCxxopts = getDependencyOption('CXXOPTS')
withIsal = getDependencyOption('ISAL')
withRpmalloc = getDependencyOption('RPMALLOC')
withZlib = getDependencyOption('ZLIB')

if withCxxopts == 'disable':
    print("[Warning] Cxxopts can not be disabled! Will enable it.")
    withCxxopts = 'enable'

# ISA-l does not compile on 32-bit because it contains statements such as [bits 64].
# It simply is not supported and I also don't see a reason. 32-bit should be long dead exactly
# like almost all (96% according to Steam) PCs have AVX support.
# On aarch64 macOS, ISA-L causes: ImportError: dlopen(python3.10/site-packages/rapidgzip.cpython-310-darwin.so, 0x0002):
#   symbol not found in flat namespace '_decode_huffman_code_block_stateless'
# On aarch64 linux, ISA-L causes:
#   /bin/ld: external/isa-l/igzip/igzip_decode_block_stateless_01.obj: error adding symbols: file in wrong format
# -> I need to migrate the ARM build settings to CMake. ISA-L has aarch64 subfolders with assembly files.
canBuildIsal = shutil.which("nasm") is not None and platform.machine() in ['x86_64', 'AMD64']
if not canBuildIsal:
    withIsal = 'disable'

print("Final rapidgzip build configuration:")
print(f"  isal: {withIsal}")
print(f"  zlib: {withZlib}")
print(f"  rpmalloc: {withRpmalloc}")
print(f"  cxxopts: {withCxxopts}")

zlib_sources = []
if withZlib == 'enable':
    zlib_sources = ['deflate.c', 'inflate.c', 'crc32.c', 'adler32.c', 'inftrees.c', 'inffast.c', 'trees.c', 'zutil.c']
    zlib_sources = ['external/zlib/' + source for source in zlib_sources]

isal_sources = [
    # "include/igzip_lib.h",
    # "include/unaligned.h",
    "include/reg_sizes.asm",
    "include/multibinary.asm",
    "igzip/igzip_inflate.c",
    "igzip/igzip.c",
    "igzip/hufftables_c.c",
    # "igzip/igzip_checksums.h",
    "igzip/igzip_inflate_multibinary.asm",
    "igzip/igzip_decode_block_stateless_01.asm",
    "igzip/igzip_decode_block_stateless_04.asm",
    "igzip/rfc1951_lookup.asm",
    # "igzip/igzip_wrapper.h",
    # "igzip/static_inflate.h",
    "igzip/stdmac.asm",
    # Compression
    # "igzip/igzip_base_aliases.c",
    "igzip/encode_df.c",
    "igzip/igzip_deflate_hash.asm",
    "igzip/igzip_icf_base.c",
    "igzip/igzip_icf_body.c",
    "igzip/igzip_base.c",
    "igzip/igzip_body.asm",
    "igzip/igzip_multibinary.asm",
    "igzip/igzip_update_histogram_01.asm",
    "igzip/igzip_update_histogram_04.asm",
    # "igzip/igzip_update_histogram.asm",
    # "igzip/bitbuf2.asm",
    # "igzip/data_struct2.asm",
    "igzip/encode_df_04.asm",
    "igzip/encode_df_06.asm",
    # "igzip/heap_macros.asm",
    # "igzip/huffman.asm",
    # "igzip/igzip_compare_types.asm",
    "igzip/igzip_finish.asm",
    "igzip/igzip_gen_icf_map_lh1_04.asm",
    "igzip/igzip_gen_icf_map_lh1_06.asm",
    "igzip/igzip_icf_body_h1_gr_bt.asm",
    "igzip/igzip_icf_finish.asm",
    "igzip/igzip_set_long_icf_fg_04.asm",
    "igzip/igzip_set_long_icf_fg_06.asm",
    # "igzip/inflate_data_structs.asm",
    # "igzip/lz0a_const.asm",
    # "igzip/options.asm",
    "igzip/proc_heap.asm",
    # "igzip/rfc1951_lookup.asm",
    "igzip/adler32_avx2_4.asm",
    "igzip/adler32_sse.asm",
    "igzip/adler32_base.c",
    # "igzip/encode_df.c",
    "igzip/flatten_ll.c",
    # "igzip/generate_custom_hufftables.c",
    # "igzip/generate_static_inflate.c",
    "igzip/huff_codes.c",
    # "igzip/hufftables_c.c",
    # "igzip/igzip_base_aliases.c",
    # "igzip/igzip_base.c",
    # "igzip/igzip_icf_base.c",
    # "igzip/igzip_icf_body.c",
    # "igzip/proc_heap_base.c",
    "crc/crc_multibinary.asm",
    "crc/crc32_gzip_refl_by16_10.asm",
    "crc/crc32_gzip_refl_by8_02.asm",
    "crc/crc32_gzip_refl_by8.asm",
    "crc/crc_base.c",
    # "crc/crc_base_aliases.c",
]
isal_sources = ['external/isa-l/' + source for source in isal_sources] if withIsal == 'enable' else []

include_dirs = [
    '.',
    'core',
    'huffman',
    'rapidgzip',
    'rapidgzip/chunkdecoding',
    'rapidgzip/huffman',
    'rapidgzip/gzip',
    'indexed_bzip2',
]
isal_includes = ['external/isa-l/include', 'external/isa-l/igzip']
if withIsal == 'enable':
    include_dirs += isal_includes
if withZlib == 'enable':
    include_dirs += ['external/zlib']
if withRpmalloc == 'enable':
    include_dirs += ['external/rpmalloc/rpmalloc']
if withCxxopts == 'enable':
    include_dirs += ['external/cxxopts/include']

rpmalloc_sources = ['external/rpmalloc/rpmalloc/rpmalloc.c'] if withRpmalloc == 'enable' else []

extensions = [
    Extension(
        # fmt: off
        name         = 'rapidgzip',
        sources      = ['rapidgzip.pyx'] + zlib_sources + isal_sources + rpmalloc_sources,
        include_dirs = include_dirs,
        language     = 'c++',
        # fmt: on
    ),
]

if cythonize:
    extensions = cythonize(extensions, compiler_directives={'language_level': '3'})


def supportsFlag(compiler, flag):
    with tempfile.NamedTemporaryFile('w', suffix='.cpp') as file:
        file.write('int main() { return 0; }')
        try:
            compiler.compile([file.name], extra_postargs=[flag])
        except CompileError:
            print("[Info] Compiling with argument failed. Will try another one. The above error can be ignored!")
            return False
    return True


def hasInclude(compiler, systemInclude):
    with tempfile.NamedTemporaryFile('w', suffix='.cpp') as file:
        file.write(f'#include <{systemInclude}>\n' + 'int main() { return 0; }')
        try:
            compiler.compile([file.name])
        except CompileError:
            print(
                f"[Info] Check for {systemInclude} system header failed. Will try without out it. "
                "The above error can be ignored!"
            )
            return False
    return True


# https://github.com/cython/cython/blob/master/docs/src/tutorial/appendix.rst#python-38
class Build(build_ext):
    def build_extensions(self):
        # This is as hacky as it gets just in order to have different compile arguments for the zlib C-code as
        # opposed to the C++ code but I don't see another way with this subpar "build system" if you can call
        # it even that.
        oldCompile = self.compiler.compile

        if withIsal == 'disable':
            nasmCompiler = None
        elif sys.platform == "win32":
            from nasm_extension.winnasmcompiler import WinNasmCompiler

            nasmCompiler = WinNasmCompiler(verbose=True)
        else:
            from nasm_extension.nasmcompiler import NasmCompiler

            nasmCompiler = NasmCompiler(verbose=True)

        def newCompile(sources, *args, **kwargs):
            cSources = [source for source in sources if source.endswith('.c')]
            asmSources = [source for source in sources if source.endswith('.asm')]
            cppSources = [source for source in sources if not source.endswith('.c') and not source.endswith('.asm')]

            objects = []
            if asmSources and nasmCompiler:
                nasm_kwargs = copy.deepcopy(kwargs)
                nasm_kwargs['extra_postargs'] = []
                nasm_kwargs['include_dirs'] = isal_includes

                # One of the crudest hacks ever. But for some reason, I get:
                # fatal: unable to open include file `reg_sizes.asm'
                # But this only happens with the manylinux2014 image. Building with cibuildwheel and manylinux2_28
                # works perfectly fine without this hack. Maybe it is a problem with 2.10.07-7.el7 (manylinux2014),
                # which has been fixed in 2.15.03-3.el8 (manylinux_2_28).
                # @see https://www.nasm.us/xdoc/2.16.01/html/nasmdocc.html#section-C.1.32
                # The only mention of -I, which I could find, is in 2.14:
                # > Changed -I option semantics by adding a trailing path separator unconditionally.
                # It even fails to include .asm files in the same directory as the .asm file to be compiled.
                #   nasm -I. -Iexternal/isa-l/include -Iexternal/isa-l/igzip -f elf64 \
                #       external/isa-l/igzip/igzip_decode_block_stateless_01.asm -o \
                #       build/temp.linux-x86_64-3.6/external/isa-l/igzip/igzip_decode_block_stateless_01.obj
                # external/isa-l/igzip/igzip_decode_block_stateless_01.asm:3: fatal: unable to open include file
                #   `igzip_decode_block_stateless.asm'
                # error: command 'nasm' failed with exit status 1
                # I even tried changing -I<dir> to -I <dir> by overwriting nasmcompiler._setup_compile but to no avail.
                for path in isal_includes:
                    for fileName in os.listdir(path):
                        if fileName.endswith('.asm'):
                            shutil.copy(os.path.join(path, fileName), ".")
                objects.extend(nasmCompiler.compile(asmSources, *args, **nasm_kwargs))

            if cppSources:
                objects.extend(oldCompile(cppSources, *args, **kwargs))

            if cSources:
                cppCompileArgs = [
                    '-fconstexpr-ops-limit=99000100',
                    '-fconstexpr-steps=99000100',
                    '-std=c++17',
                    '/std:c++17',
                ]
                if 'extra_postargs' in kwargs:
                    kwargs['extra_postargs'] = [x for x in kwargs['extra_postargs'] if x not in cppCompileArgs]
                objects.extend(oldCompile(cSources, *args, **kwargs))

            return objects

        self.compiler.compile = newCompile

        for ext in self.extensions:
            ext.extra_compile_args = [
                '-std=c++17',
                '-O3',
                '-DNDEBUG',
                '-DWITH_PYTHON_SUPPORT',
                '-D_LARGEFILE64_SOURCE=1',
                '-D_GLIBCXX_ASSERTIONS',
            ]

            if supportsFlag(self.compiler, '-flto=auto'):
                ext.extra_compile_args += ['-flto=auto']
            elif supportsFlag(self.compiler, '-flto'):
                ext.extra_compile_args += ['-flto']

            if supportsFlag(self.compiler, '-D_FORTIFY_SOURCE=2'):
                ext.extra_compile_args += ['-D_FORTIFY_SOURCE=2']
            if withRpmalloc != 'disable':
                ext.extra_compile_args.append('-DWITH_RPMALLOC')
            if withIsal != 'disable':
                ext.extra_compile_args.append('-DWITH_ISAL')

            # https://github.com/cython/cython/issues/2670#issuecomment-432212671
            # https://github.com/cython/cython/issues/3405#issuecomment-596975159
            # https://bugs.python.org/issue35037
            # https://bugs.python.org/issue4709
            if platform.system() == 'Windows' and platform.machine().endswith('64'):
                ext.extra_compile_args += ['-DMS_WIN64']

            if self.compiler.compiler_type == 'mingw32':
                ext.extra_link_args = [
                    '-static-libgcc',
                    '-static-libstdc++',
                    '-Wl,-Bstatic,--whole-archive',
                    '-lwinpthread',
                    '-Wl,--no-whole-archive',
                ]

            elif self.compiler.compiler_type == 'msvc':
                ext.extra_compile_args = [
                    '/std:c++17',
                    '/O2',
                    '/DNDEBUG',
                    '/DWITH_PYTHON_SUPPORT',
                    '/constexpr:steps99000100',
                ]
                if withRpmalloc != 'disable':
                    ext.extra_compile_args.append('/DWITH_RPMALLOC')
                if withIsal != 'disable':
                    ext.extra_compile_args.append('/DWITH_ISAL')
                if withRpmalloc != 'disable':
                    # This list is from rpmalloc/build/ninja/msvc.py
                    ext.libraries = ['kernel32', 'user32', 'shell32', 'advapi32']

            else:
                # The default limit is ~33 M (1<<25) and 99 M seem to be enough to compile currently on GCC 11.
                if supportsFlag(self.compiler, '-fconstexpr-ops-limit=99000100'):
                    ext.extra_compile_args += ['-fconstexpr-ops-limit=99000100']
                elif supportsFlag(self.compiler, '-fconstexpr-steps=99000100'):
                    ext.extra_compile_args += ['-fconstexpr-steps=99000100']

                if sys.platform == 'linux':
                    ext.extra_compile_args += ['-D_GNU_SOURCE']

                if sys.platform.startswith('darwin') and supportsFlag(self.compiler, '-mmacosx-version-min=10.15'):
                    ext.extra_compile_args += ['-mmacosx-version-min=10.15']
                    ext.extra_link_args += ['-mmacosx-version-min=10.15']

                # Add some hardening. See e.g.:
                # https://www.phoronix.com/news/GCC-fhardened-Hardening-Option
                # https://developers.redhat.com/blog/2018/03/21/compiler-and-linker-flags-gcc
                # I have not observed any performance impact for these.
                ext.extra_compile_args += ['-fstack-protector-strong']
                ext.extra_link_args += ['-fstack-clash-protection']
                # AppleClang seems to not like this flag:
                if supportsFlag(self.compiler, '-fcf-protection=full'):
                    ext.extra_compile_args += ['-fcf-protection=full']

            if hasInclude(self.compiler, 'unistd.h'):
                ext.extra_compile_args += ['-DZ_HAVE_UNISTD_H']

        super(Build, self).build_extensions()


setup(
    # fmt: off
    ext_modules = extensions,
    cmdclass    = {'build_ext': Build},
    # fmt: on
)
