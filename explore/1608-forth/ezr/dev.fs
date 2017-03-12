\ try out ZDI access to EZ80

compiletoram? [if]  forgetram  [then]

PB4 constant ZCL
PB5 constant ZDA

: ez80-8MHz ( -- )
  7200 pb0 pwm-init   \ first set up pwm correctly
  8 3 timer-init      \ then mess with the timer divider, i.e. ÷9
  9998 pb0 pwm        \ finally, set the pwm to still toggle
;

: zdi-init ( -- )
  ZDA ios!  OMODE-OD ZDA io-mode!
  ZCL ios!  OMODE-PP ZCL io-mode! ;

: zcl-lo  10 us ZCL ioc!  10 us ;
: zcl-hi  10 us ZCL ios!  10 us ;

: zdi! ( f -- )  zcl-lo  ZDA io!  zcl-hi  ZDA ios! ;

: zdi-start ( u -- )
  ( zcl-hi ) ZDA ioc!
  7 0 do
    dup $40 and zdi!  shl
  loop  drop ;

: zdi> ( addr -- val )
  zdi-start  1 zdi!  1 zdi!
  0  8 0 do
    zcl-lo  zcl-hi
    shl  ZDA io@ 1 and or
  loop
  zcl-lo ZDA ios! zcl-hi ;

: >zdi ( val addr -- )
  zdi-start  0 zdi!  1 zdi!
  8 0 do
    dup $80 and zdi!  shl
  loop  drop
  zcl-lo ZDA ios! zcl-hi ;

: z  0 zdi> h.2 space  1 zdi> h.2 space  2 zdi> h.2 space ;

: s  3 zdi>
  dup 7 bit and if ." zdi " then
  dup 5 bit and if ." halt " then
  dup 4 bit and if ." adl " then
  dup 3 bit and if ." madl " then
  dup 2 bit and if ." ief1 " then
             0= if ." <run> " then ;

: b  7 bit $10 >zdi ;

: c  0 $10 >zdi ;

: r1 ( u -- )  $16 >zdi  [char] : emit  $11 zdi> h.2  $10 zdi> h.2  space ;
: r  ." FA" 0 r1  ." BC" 1 r1  ." DE" 2 r1  ." HL" 3 r1
     ." IX" 4 r1  ." IY" 5 r1  ." SP" 6 r1  ." PC" 7 r1 ;

: ?
  cr ." z = show chip info "
  cr ." s = show processor status "
  cr ." b = break next "
  cr ." c = continue "
  cr ." r = show registers "
;

ez80-8MHz  zdi-init  100 ms  ?