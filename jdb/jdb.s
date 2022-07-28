;; JDB -- Jaguar debug stub
;;
;; This works with jserve through EZ HOST memory to present a GDB stub

regs	EQU	$2C00

errno	EQU	$2FF0
errf1	EQU	$2FF4	; Bus error frame info (like we need this!)
errf2	EQU	$2FF8

BORD1	EQU	$F0002A
BG	EQU	$F00058

HPION	EQU	$4004
HPIOFF	EQU	$4001

	.text
	
; Initialize (clear out regs, set up exceptions)
init:
	move.w	#0, errno
	move.w	#44, d1			; Clear regs: 180 bytes = 45 words
	move.l	#regs, a0
initloop:
	move.l	#0, (a0)+
	dbra	d1, initloop

cleanup:
	move.l	#$7F000, d1		; Clear all words from $4000 on
	move.l	#$4000, a0		; C expects BSS you know!
clearmem:
	move.l	#0, (a0)+
	subq.l	#1, d1
	bne	clearmem

; Set up safe values for downloaded code
	move.l	#$1FFFF0, sp		; Stack pointer at top of stack
	move.l	sp, 60+regs	
;;;	move.l	#$1FFFF4, 56+regs	; Last frame pointer starts just before return address
;;;	move.l	#endpgm, (sp)		; Return address just breaks GDB
	move.w	#$2000, 66+regs		; Supervisor mode ON and interrupts enabled
	move.l	#$4000, 68+regs	

	move.w	#15, d1			; Set first 16 exception vectors
	move.l	#0, a0
	move.l	#ex0, d0
vecloop:
	move.l	d0, (a0)+
	subq.l	#6, d0			; Each vector is 6 bytes apart
	dbra	d1, vecloop

	move.w	#239, d1		; Set next 240 exception vectors to vector 1 (unknown)
vecloop2:
	move.l	#ex1, (a0)+
	dbra	d1, vecloop2

	move.l	#ex0, $84		; Breakpoint

; Start parsing commands
parse:
	move.l	#$800000, a4		; a4 = HPI write data
	move.l	#$C00000, a5		; a5 = HPI write address, read data

nextcmd:
	move.w	#HPION, (a5)		; Enter HPI write mode
	move.w	#$3808, (a5)		; State = waiting for command
	move.w	#-1, (a4)
	move.w	errno, (a4)		; Store trap number (68K style)
	move.w	#HPIOFF, (a5)		; Enter Flash read-only mode

	move.l  #$FF00,BORD1        	; Blue means waiting
	move.w  #$FF00,BG

waitforcmd:
	move.w	#$3808, (a5)		; Poll for positive
	move.w	(a5), d0
	cmp.w	#0, d0
	blt 	waitforcmd
	cmp.w	#12, d0
	bgt	waitforcmd

	move.l  #$0,BORD1        	; Black means working
	move.w  #$0,BG

	move.l	#cmdtable, a0		; Get instruction and run it
	add.w	d0, a0
	move.l	(a0), a0	
	jmp	(a0)

endpgm:
	move.w	#-1, errno		; Program ended normally
	jmp cleanup			; Clean up and start over

write:
	move.w	#$3800, (a5)		; Point at command
	move.w	(a5), d0		; EZ base address
	move.w	(a5), d1		; Length of transfer in words
	move.l	(a5), a0		; 68K base address

	move.w	d0, (a5)		; Point at EZ base
writeloop:
	move.w	(a5), (a0)+
	dbra	d1, writeloop

	jmp nextcmd			; All done

; to allow reading from ROM, cartridge is a little tricker
; here we read and write to a small RAM buffer to speed it
; up a little bit.
; Updated 6/16/2016 by Tursi
read:
	move.w	#$3800, (a5)		; Point at command
	move.w	(a5), d0				; EZ base address
	move.w	(a5), d1				; Length of transfer in words
	move.w	d1,d2						; make copy for write loop
	move.l	(a5), a0				; 68K base address
	move.w	#HPIOFF, (a5)		; Enter Flash read-only mode

readloopA:
	movea #readbuf,a1				; set up a read from ROM (maybe) to the buffer
	move.w d1,d3						; get remaining read count
	cmpi.w #$3,d3						; test against max
	bls.s readloopB					; jump ahead if small enough
	move.w #$3,d3						; max of 4 words

readloopB:
	move.w (a0)+,(a1)+			; copy to buffer, up to 4 words
	subq #1,d1							; count down remaining reads
	dbra d3,readloopB				; loop variable
	
	move.w	#HPION, (a5)		; Enter HPI write mode
	move.w	d0, (a5)				; Point at EZ base
	movea #readbuf,a1				; set up a read from buffer to HPI
	move.w d2,d3						; get remaining write count
	cmpi.w #$3,d3						; test against max
	bls.s readloopC					; jump ahead if small enough
	move.w #$3,d3						; max of 4 words
	
readloopC:
	move.w	(a1)+, (a4)			; copy from buffer to HPI
	addq #2,d0							; increment	HPI address
	subq #1,d2							; count down remaining writes
	dbra	d3, readloopC			; loop variable
	
	move.w	#HPIOFF, (a5)		; done loop, enter Flash read-only mode
	
	cmp.w #-1,d1						; are we done all reads?
	bne.s readloopA					; if not, loop around

	jmp nextcmd							; All done

run:
	move.w	#$3802, (a5)		; Point at command
	
	move.w	regs+66, d1			; Clear trace flag (single step)
	and.w	#$7fff, d1
	or.w	(a5), d1					; Add trace flag back from run command

	move.l	(a5), d6				; New PC (or 0 for current)
	tst.l	d6								; 0 means use current PC
	bne	newpc

	move.l	regs+68, d6			; Load PC from GDB regs

newpc:
	move.w	#HPION, (a5)		; Enter HPI write mode
	move.w	#-2, (a4)				; State = running
	move.w	#HPIOFF, (a5)		; Enter Flash read-only mode

; Warning -- we are not handling user mode properly here

	move.l	regs+60, sp			; Restore stack pointer
	move.l	d6, -(sp)				; Push program counter
	move.w	d1, -(sp)				; Push status word

	movem.l	regs, d0-d7/a0-a6	; Restore registers
	rte											; Return to user code!

; These are entry points for the various traps
; 2-15 are real traps, 0 is breakpoint/software generated, 1 is all other ints (unknown)
ex15:	addq.w	#1, trapno
ex14:	addq.w	#1, trapno
ex13:	addq.w	#1, trapno
ex12:	addq.w	#1, trapno
ex11:	addq.w	#1, trapno
ex10:	addq.w	#1, trapno
ex9:	addq.w	#1, trapno
ex8:	addq.w	#1, trapno
ex7:	addq.w	#1, trapno
ex6:	addq.w	#1, trapno
ex5:	addq.w	#1, trapno
ex4:	addq.w	#1, trapno
ex3:	addq.w	#1, trapno
ex2:	addq.w	#1, trapno
ex1:	addq.w	#1, trapno
ex0:

; This is our multipurpose exception handler
	move.l  #$FF00,BORD1        	; Green means made it to handler
	move.w  #$FF00,BG

	movem.l	d0-d7/a0-a6, regs	; Save program state

	move.l	#regs, a5		; Point to regs

	move.w	trapno, d2		; Check exception number
	move.w	d2, errno		; Save details in error handler
	move.w	#0, trapno		; Reset for next exception

	cmp.w	#2, d2			; Bus error (2) or address error (3)?
	beq	exbuserr		; These have funky frames
	cmp.w	#3, d2			
	bne	exnormal		

exbuserr:
	move.l	(sp)+, errf1		; 7 word exception
	move.l	(sp)+, errf2		; Pop these away for debugging

exnormal:
	move.w	(sp)+, 66(a5)		; Pop status register
	move.l	(sp)+, 68(a5)		; Pop program counter

; Warning -- we are not handling user mode properly here
        move.l	sp, 60(a5)      	; Save super a7/sp

	cmp.w	#0, d2			; Breakpoint means PC -= 2
	bne	exnobp

	subq.l	#2, 68(a5)
exnobp:	
	jmp parse			; Start parsing commands

cmdtable:
	.dc.l	nextcmd			; Ping (do nothing)
	.dc.l	write			; Write buffer
	.dc.l	read			; Read buffer
	.dc.l	run			; Start running

trapno:
	.dc.w	0

readbuf:
	.dc.w	2,4,6,8

	.end

