#====================================================================
#       Macro & Assembler flags
#====================================================================

STADDR = 1000
MACFLAGS = +o0 +o1 +o2 -fb
ALNFLAGS = -w -v -e -rd -a $(STADDR) x 2800

#====================================================================
#       Default Rules
#====================================================================
.SUFFIXES:      .o .s

.s.o:
	rmac $(MACFLAGS) $<

#====================================================================
#       EXECUTABLES
#====================================================================

OBJ = jdb.o
 
jdb.cof: $(OBJ)
	rln $(ALNFLAGS) -o jdb.cof $(OBJ)

#############################################################################

jdb.o: jdb.s 

clean:
	$(RM) $(OBJ) jdb.cof
