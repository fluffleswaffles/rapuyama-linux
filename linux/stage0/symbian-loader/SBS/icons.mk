	#
# Copyright (c) 2007 Nokia Corporation and/or its subsidiary(-ies).
# All rights reserved.
# This component and the accompanying materials are made available
# under the terms of "Eclipse Public License v1.0"
# which accompanies this distribution, and is available
# at the URL "http://www.eclipse.org/legal/epl-v10.html".
#
# Initial Contributors:
# Nokia Corporation - initial contribution.
#
# Contributors:
#
# Description:  
#

ifeq (WINS,$(findstring WINS, $(PLATFORM)))
ZDIR=$(EPOCROOT)\epoc32\release\$(PLATFORM)\$(CFG)\z
else
ZDIR=$(EPOCROOT)\epoc32\data\z
endif

TARGETDIR=$(ZDIR)\resource\apps
ICONTARGETFILENAME=$(TARGETDIR)\LinuxE5.mif

ICONSOURCEFILENAME=linuxe5.svg

do_nothing :
	@rem do_nothing

MAKMAKE : do_nothing

BLD : do_nothing

CLEAN : 
	@echo ...Deleting $(ICONTARGETFILENAME)
	del /q /f $(ICONTARGETFILENAME)
LIB : do_nothing

CLEANLIB : do_nothing

RESOURCE : $(ICONTARGETFILENAME)

$(ICONTARGETFILENAME) : $(ICONSOURCEFILENAME)
	mifconv $(ICONTARGETFILENAME) \
		/c8,8 $(ICONSOURCEFILENAME)

FREEZE : do_nothing

SAVESPACE : do_nothing

RELEASABLES :
	@echo $(ICONTARGETFILENAME)

FINAL : do_nothing 
