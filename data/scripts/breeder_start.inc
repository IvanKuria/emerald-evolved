@ Emerald Evolved: the Breeder start (see src/breeder_start.c). Runs once, right after the game begins outside the
@ Route 117 Day Care.
EventScript_BreederStart::
	lockall
	msgbox Text_BreederStart_Welcome, MSGBOX_DEFAULT
	closemessage
	special BreederChooseStarter
	delay 2
	msgbox Text_BreederStart_Eevees, MSGBOX_DEFAULT
	releaseall
	end

Text_BreederStart_Welcome:
	.string "Welcome to ROUTE 117!\p"
	.string "The POKéMON DAY CARE is right here,\n"
	.string "so you can start breeding right away.\p"
	.string "First, pick a partner to travel with!$"

Text_BreederStart_Eevees:
	.string "The DAY CARE also sent two EEVEE\n"
	.string "along: one boy and one girl.\p"
	.string "Leave them with the DAY CARE couple\n"
	.string "and see what their babies look like!$"
