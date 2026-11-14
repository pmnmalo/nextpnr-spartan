module top(input clk, output reg [3:0] led);
  reg [17:0] x = 0, y = 0; reg [35:0] p;
  always @(posedge clk) begin x <= x + 3; y <= y + 5; p <= x * y; led <= p[35:32]; end
endmodule
