`timescale 1ns/1ns

// Reshape a PSX-native button vector into Minimig's joystick order, so the
// CD32 pad shift registers in userio.v consume it unchanged.
//
// Pure combinational: no clock, no state. Its whole job is a wiring decision
// that is easy to get wrong and impossible to spot at runtime.
module snac_cd32 (
	input  [15:0] psx,
	output [10:0] joy
);

// Directions pass straight through; buttons are reordered to CD32's.
// Bit 4 is the plain-joystick fire (userio.v:334), which on a CD32 pad is Red,
// so Cross lands there -- the button under the thumb in both worlds.
assign joy = { psx[10],   // [10] Play/Pause  <- Start
               psx[8],    //  [9] Reverse     <- L1
               psx[9],    //  [8] Forward     <- R1
               psx[7],    //  [7] Green       <- Triangle
               psx[6],    //  [6] Yellow      <- Square
               psx[5],    //  [5] Blue        <- Circle
               psx[4],    //  [4] Red / fire  <- Cross
               psx[3],    //  [3] Up
               psx[2],    //  [2] Down
               psx[1],    //  [1] Left
               psx[0] };  //  [0] Right

endmodule
