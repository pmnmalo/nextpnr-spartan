// The switches added and registered, the sum on the LEDs: a carry chain between I/Os, a register stage.
module top(input clk, input [3:0] sw, output [3:0] led);
  reg [3:0] a = 0, sum = 0;
  always @(posedge clk) begin
    a <= sw;
    sum <= sum + a;
  end
  assign led = sum;
endmodule
