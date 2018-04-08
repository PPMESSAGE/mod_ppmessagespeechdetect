include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_ppmessagespeechdetect

mod_LTLIBRARIES = mod_ppmessagespeechdetect.la
mod_ppmessage_la_SOURCES  = mod_ppmessagespeechdetect.c
mod_ppmessage_la_CFLAGS   = $(AM_CFLAGS)
mod_ppmessage_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_ppmessage_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

