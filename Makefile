ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif
include $(DEVKITARM)/3ds_rules

TARGET  := browser3ds
CTRULIB := $(DEVKITPRO)/libctru
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections -fdata-sections $(ARCH) -I$(CTRULIB)/include -Isource -D__3DS__
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(TARGET).map -L$(CTRULIB)/lib
LIBS    := -lctru -lm

.PHONY: all clean
all: $(TARGET).3dsx $(TARGET).cia

source/main.o: source/main.c source/font.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: source/main.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).smdh: icon.png
	smdhtool --create "Browser3DS" "Web browser using a Supabase proxy" "Colling Studio" icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	3dsxtool $< $@ --smdh=$(TARGET).smdh

banner.bnr: banner.png silent.wav
	bannertool makebanner -i banner.png -a silent.wav -o $@

icon.icn: icon.png
	bannertool makesmdh -s "Browser3DS" -l "Web browser using a Supabase proxy" -p "Colling Studio" -i icon.png -o $@

$(TARGET).cia: $(TARGET).elf app.rsf banner.bnr icon.icn
	makerom -f cia -o $@ -elf $(TARGET).elf -rsf app.rsf -icon icon.icn -banner banner.bnr -exefslogo -target t

clean:
	rm -f source/*.o *.elf *.3dsx *.cia *.smdh *.bnr *.icn *.map
