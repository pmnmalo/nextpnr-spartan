module t(input clk, input we, input [7:0] a, input [7:0] b, input [3:0] d, output [15:0] q);
  reg m32 [0:31]; reg m64 [0:63]; reg m128 [0:127]; reg m256 [0:255];
  reg [3:0] q4 [0:63]; reg [1:0] r2 [0:31];
  reg dp [0:63];
  always @(posedge clk) if (we) begin
    m32[a[4:0]] <= d[0]; m64[a[5:0]] <= d[1]; m128[a[6:0]] <= d[2]; m256[a] <= d[3];
    q4[a[5:0]] <= d; r2[a[4:0]] <= d[1:0]; dp[a[5:0]] <= d[3] ^ d[0];
  end
  assign q = {m32[a[4:0]], m64[a[5:0]], m128[a[6:0]], m256[a], q4[b[5:0]], r2[b[4:0]], r2[a[4:0]], dp[b[5:0]], dp[a[5:0]]};
endmodule
