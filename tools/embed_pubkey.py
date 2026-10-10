# Embute public.key no firmware ESP8266 antes de o core compilar.
#
# O PlatformIO não executa o recipe.hooks do arduino-cli. Sem este
# header, Updater_Signing.h do core define ARDUINO_SIGNING 0, Update.end()
# não remove a assinatura e o eboot lê o tamanho da assinatura como se
# fosse o tamanho descomprimido.
#
# Tem de ser extra_scripts pre: e post:. O pre: coloca o -I antes de o
# construtor clonar o ambiente da biblioteca Wi-Fi. O post: liga a
# checagem ao firmware.elf já criado. Os dois são obrigatórios.
Import("env")

import os
import subprocess
import sys

project = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
if not os.path.isabs(build_dir):
    build_dir = os.path.join(project, build_dir)
out_dir = os.path.abspath(os.path.join(build_dir, "signing"))
header = os.path.join(out_dir, "Updater_Signing.h")
pubkey = os.path.join(project, "public.key")
framework = env.PioPlatform().get_package_dir("framework-arduinoespressif8266")
signing_py = os.path.join(framework, "tools", "signing.py")

os.makedirs(out_dir, exist_ok=True)
if not os.path.isfile(pubkey):
    sys.stderr.write("public.key ausente; o OTA não pode verificar assinatura\n")
    env.Exit(1)
if not os.path.isfile(signing_py):
    sys.stderr.write("signing.py do core ESP8266 não encontrado\n")
    env.Exit(1)

subprocess.check_call(
    [sys.executable, signing_py, "--mode", "header",
     "--publickey", pubkey, "--out", header]
)

with open(header, "r", encoding="utf-8") as handle:
    generated = handle.read()
if "#define ARDUINO_SIGNING 1" not in generated or "signing_pubkey" not in generated:
    sys.stderr.write("Updater_Signing.h não habilita a assinatura\n")
    env.Exit(1)

# pre: ainda vê PROGNAME "program". O builder da plataforma troca para
# "firmware" antes dos scripts post:.
late = env.get("PROGNAME", "program") not in ("", "program")

if not env.get("OTA_PUBKEY_PATH"):
    if late:
        sys.stderr.write(
            "OTA: tools/embed_pubkey.py tem de ser pre: antes do core\n")
        env.Exit(1)
    # Primeiro da lista de -I. cores/esp8266 também tem Updater_Signing.h
    # e o dele desliga a assinatura; ele não pode ganhar a busca.
    env.Prepend(CPPPATH=[out_dir])
    env.Replace(OTA_PUBKEY_PATH=out_dir)


def _require_verifier(source, target, env):
    del source
    elf = str(target[0])
    if not os.path.isfile(elf):
        sys.stderr.write("OTA: ELF ausente, assinatura não confirmada\n")
        env.Exit(1)
    nm = os.path.join(
        env.PioPlatform().get_package_dir("toolchain-xtensa"),
        "bin", "xtensa-lx106-elf-nm")
    if not os.path.isfile(nm):
        sys.stderr.write("OTA: xtensa-lx106-elf-nm não encontrado\n")
        env.Exit(1)
    symbols = subprocess.check_output(
        [nm, "-C", elf], text=True, errors="replace")
    missing = [
        name for name in ("updaterSigningVerifier", "updaterSigningHash")
        if name not in symbols
    ]
    if missing:
        sys.stderr.write(
            "OTA: firmware sem " + ", ".join(missing)
            + ". A chave não entrou no Updater.\n")
        env.Exit(1)
    pem = open(pubkey, "rb").read()
    blob = open(elf, "rb").read()
    body = b"\n".join(
        line for line in pem.splitlines()
        if line and not line.startswith(b"-----"))
    if pem not in blob and body not in blob:
        sys.stderr.write("OTA: public.key não está dentro do firmware\n")
        env.Exit(1)
    # O sketch e o Updater têm de concordar. Sem este literal o aparelho
    # baixaria o gzip puro e Update.end() recusaria a imagem.
    if b"firmware.signed.bin" not in blob:
        sys.stderr.write(
            "OTA: o sketch não pede firmware.signed.bin; "
            "ARDUINO_SIGNING não chegou no OTAService\n")
        env.Exit(1)
    print("OTA: updaterSigningVerifier, public.key e firmware.signed.bin estão no firmware")


if late and not env.get("OTA_SIGNING_CHECK"):
    env.Replace(OTA_SIGNING_CHECK=True)
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _require_verifier)
