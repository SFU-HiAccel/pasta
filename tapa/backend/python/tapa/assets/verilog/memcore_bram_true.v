`default_nettype none

module memcore_bram_true #(
  parameter DATA_WIDTH = 32,
  parameter ADDRESS_WIDTH = 6,
  parameter ADDRESS_RANGE = 64
) (

  // memory port 1
  input wire [ADDRESS_WIDTH-1:0] address0,
  input wire                     ce0,
  input wire [DATA_WIDTH-1:0]    d0,
  input wire                     we0,
  output reg [DATA_WIDTH-1:0]    q0,

  // memory port 2
  input wire [ADDRESS_WIDTH-1:0] address1,
  input wire                    ce1,
  input wire [DATA_WIDTH-1:0]    d1,
  input wire                    we1,
  output reg [DATA_WIDTH-1:0]    q1,
  input wire                    reset,
  input wire                    clk
);

  (* ram_style = "block", cascade_height = 16 *)reg [DATA_WIDTH-1:0] ram[0:ADDRESS_RANGE-1];

  always @(posedge clk)
  begin
      if (ce0) begin
          if (we0)
              ram[address0] <= d0;
          else
              q0 <= ram[address0];
      end
  end
  always @(posedge clk)
  begin
      if (ce1) begin
          if (we1)
              ram[address1] <= d1;
          else
              q1 <= ram[address1];
      end
  end
endmodule

`default_nettype wire
