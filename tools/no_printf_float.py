# Tira o suporte a float de printf/scanf do link (≈16 KB de flash).
#
# O builder do core força "-u _printf_float -u _scanf_float"; o sketch
# não formata nem lê float por printf/scanf (ArduinoJson tem conversão
# própria). O CI faz o mesmo com --build-property build.float=.
#
# PIO: extra_scripts post: (Import("env")).
Import("env")  # noqa: F821

flags = env["LINKFLAGS"]  # noqa: F821
for sym in ("_printf_float", "_scanf_float"):
    while sym in flags:
        i = flags.index(sym)
        del flags[i - 1:i + 1]  # remove o "-u" junto
