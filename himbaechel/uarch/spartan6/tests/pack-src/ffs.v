module t(input clk, input g, input rst, input set, input en, input [7:0] d, output reg [7:0] q, output reg [7:0] n, output reg [3:0] l, inout [3:0] io, input oe, output z);
  always @(posedge clk or posedge rst) if (rst) q[3:0] <= 0; else if (en) q[3:0] <= d[3:0];
  always @(posedge clk or posedge set) if (set) q[7:4] <= 4'hf; else q[7:4] <= d[7:4] ^ q[3:0];
  always @(negedge clk) if (rst) n <= 0; else if (en) n <= d + q;
  always @* if (g) l = d[3:0] & d[7:4];
  assign io = oe ? d[3:0] : 4'bz;
  assign z = ^io;
endmodule
