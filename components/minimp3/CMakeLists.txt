# minimp3 (lieff, CC0) - the MP3 decoder of the system player
# (services/tab5_musica). Its own component, not a loose file in main/:
# it is third-party code, with its licence alongside, and only the service uses it.
idf_component_register(SRCS "minimp3_impl.c"
                       INCLUDE_DIRS ".")
