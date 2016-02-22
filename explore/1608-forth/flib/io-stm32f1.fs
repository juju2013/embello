\ I/O pin primitives

$40010800 constant GPIO-BASE
      $00 constant GPIO.CRL   \ Reset $44444444 Port Conf Register Low
      $04 constant GPIO.CRH   \ Reset $44444444 Port Conf Register High
      $08 constant GPIO.IDR   \ RO              Input Data Register
      $0C constant GPIO.ODR   \ Reset 0         Output Data Register
      $10 constant GPIO.BSRR  \ Reset 0         Port Bit Set/Reset Reg
      $14 constant GPIO.BRR   \ Reset 0         Port Bit Reset Register

: io ( port# pin# -- pin )  \ combine port and pin into single int
  swap 8 lshift or  2-foldable ;
: io# ( pin -- u )  \ convert pin to bit position
  $1F and  1-foldable ;
: io-mask ( pin -- u )  \ convert pin to bit mask
  1 swap io# lshift  1-foldable ;
: io-port ( pin -- u )  \ convert pin to port number (A=0, B=1, etc)
  8 rshift  1-foldable ;
: io-base ( pin -- addr )  \ convert pin to GPIO base address
  $F00 and 2 lshift GPIO-BASE +  1-foldable ;
: io@ ( pin -- u )  \ get pin value (0 or 1)
  dup io-base GPIO.IDR + @ swap io# rshift 1 and ;
: io-0! ( pin -- )  \ clear pin to low
  dup io-mask swap io-base GPIO.BRR + ! ;
: io-1! ( pin -- )  \ set pin to high
  dup io-mask swap io-base GPIO.BSRR + ! ;
: io! ( f pin -- )  \ set pin value
  \ use upper 16 bits in BSRR to reset with same operation
  swap 0= $10 and + io-1! ;
: iox! ( pin -- )  \ toggle pin
  dup io@ 0= swap io! ;

%0000 constant IMODE-ADC
%0100 constant IMODE-OPEN
%1000 constant IMODE-PULL

%0001 constant OMODE-PP
%0101 constant OMODE-OD
%1001 constant OMODE-AF-PP
%1101 constant OMODE-AF-OD

  %01 constant OMODE-SLOW  \ add to OMODE-* for 2 MHz iso 10 MHz drive
  %10 constant OMODE-FAST  \ add to OMODE-* for 50 MHz iso 10 MHz drive

: io-mode! ( mode pin -- )  \ set the CNF and MODE bits for a pin
  dup io-base GPIO.CRL + over 8 and shr + >r ( R: crl/crh )
  io# 7 and 4 * ( mode shift )
  $F over lshift not ( mode shift mask )
  r@ @ and -rot lshift or r> ! ;

: io. ( pin -- )  \ display readable GPIO registers associated with a pin
  cr
  ." PIN " dup io#  dup .  10 < if space then
  ." PORT " dup io-port [char] A + emit
  io-base
  ."   CRL " dup @ hex.  4 +
  ."  CRH " dup @ hex.  4 +
  ."  IDR " dup @ hex.  4 +
  ."  ODR " dup @ hex. drop ;