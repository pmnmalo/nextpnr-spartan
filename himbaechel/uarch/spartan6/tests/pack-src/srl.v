module t(input clk, input ce, input [4:0] a, input [7:0] d, output [7:0] q, output [7:0] r);
  reg [15:0] s16 [0:3];
  reg [63:0] s64;
  reg [31:0] s32 [0:3];
  integer i;
  always @(posedge clk) begin
    for (i = 0; i < 4; i = i + 1) s16[i] <= {s16[i][14:0], d[i]};
    if (ce) for (i = 0; i < 4; i = i + 1) s32[i] <= {s32[i][30:0], d[i+4]};
    s64 <= {s64[62:0], d[0] ^ d[7]};
  end
  assign q[3:0] = {s16[3][a[3:0]], s16[2][a[3:0]], s16[1][15], s16[0][a[3:0]]};
  assign q[7:4] = {s32[3][a], s32[2][a], s32[1][31], s32[0][a]};
  assign r = {s64[63], s64[a + 5'd20], s64[47], s64[31], s64[15], s64[a], s64[0], s64[a+5'd1]};
endmodule
