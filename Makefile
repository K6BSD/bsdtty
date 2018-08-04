CFLAGS	+=	-Wall -g -O3
LDLIBS	+=	-lform -lcurses -lm

.ifndef WITHOUT_OUTRIGGER
  CFLAGS += -DWITH_OUTRIGGER -Ioutrigger/ -Lor-lib
  LDLIBS += -loutrigger -lpthread
.endif

OBJS = bsdtty.o fsk_demod.o ui.o

.ifndef WITHOUT_OUTRIGGER
bsdtty: or-lib/liboutrigger.a
.endif

bsdtty: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} ${.ALLSRC} ${LDLIBS} -o ${.TARGET}

or-lib/:
	mkdir ${.TARGET}

or-lib/liboutrigger.a: or-lib/ .EXEC
	cd or-lib && cmake ../outrigger
	${MAKE} -C or-lib

clean:
	rm -f bsdtty ${OBJS}
	rm -rf or-lib
