/* Hello.rexx - a sample CNet-style ARexx door for NilBBS (Doors.cfg type = cnetrexx), and a
   template for writing your own.  NilBBS runs the script with its CNETREXX port as the host,
   so plain commands go straight to the BBS:
     TRANSMIT text     a line to the caller          SENDSTRING text  the same, no new line
     QUERY prompt      a typed line -> RESULT        GETCHAR          one key -> RESULT
     GETUSER 1         the caller's handle           (see Config/Doors.cfg for the rest)
   When the caller hangs up, input commands return ###PANIC - save anything and exit then.
   Several callers can run a door at once: if yours keeps a shared file, lock it while you
   change it (or set  single = yes  in its Doors.cfg section). */
options results

'GETUSER 1'; name = result
'TRANSMIT Hello,' name'! This is a sample ARexx door.'
'QUERY What is your favourite Amiga? '
if result = '###PANIC' then exit 0
fav = result
if fav = '' then fav = 'A500'
'TRANSMIT The' fav '- a fine choice.'
'SENDSTRING Press a key to go back to the BBS... '
'GETCHAR'
exit 0
