HERE := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT  ?= $(abspath $(HERE)/../../../../)

prebuild:
	bldmake bldfiles gcce urel
	cp -vf $(ROOT)/driver/import/iscapi.dso \
		$(EPOCROOT)/epoc32/release/armv5/lib/

build:
	abld build -v gcce urel

clean: 
	abld reallyclean gcce urel


APP_NAME=linuxe5
EXE_NAME=$(APP_NAME).exe
EXE_FP=$(EPOCROOT)/epoc32/release/gcce/urel/$(EXE_NAME)


mksis:
	PLATFORM=gcce TARGET=urel makesis -v -d$(EPOCROOT) $(APP_NAME).pkg

depoly:
	renv send "$(APP_NAME).sis" "C:\\$(APP_NAME).sis"
	renv send "$(EPOCROOT)/epoc32/release/gcce/urel/readklog.exe" "C:\\sys\\bin\\readklog.exe"

