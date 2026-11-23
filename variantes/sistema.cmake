# menu: the public GitHub base (system apps only)
# SISTEMA (SYSTEM) VARIANT - the public base: only the system apps (the
# m5_apps_sistema list in main/CMakeLists.txt), without Berry. It is what the GitHub
# copy (ferramentas/publicar.sh) builds. A new personal app is left out
# by itself, without anyone having to remember anything.
#     ./gravar.sh sistema
set(M5_BERRY OFF)
set(M5_SO_SISTEMA ON)
set(M5_SEM_APPS_VARIANTE "")
set(M5_VARIANTE_ROTULO "sistema")
