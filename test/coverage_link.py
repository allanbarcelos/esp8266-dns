# Adiciona --coverage na etapa de link do env "coverage" (libgcov).
Import("env")  # noqa: F821  (injetado pelo PlatformIO/SCons)
env.Append(LINKFLAGS=["--coverage"])  # noqa: F821
