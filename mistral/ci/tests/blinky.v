// A counter on the LEDs (its top bits): registers and a carry chain.
module top(input clk, output [3:0] led);
  reg [27:0] count = 0;
  always @(posedge clk) count <= count + 1;
  assign led = count[27:24];
endmodule
