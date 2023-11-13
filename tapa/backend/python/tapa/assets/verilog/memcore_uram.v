`default_nettype none

module memcore_uram #(
  parameter DATA_WIDTH = 32,
  parameter ADDRESS_WIDTH = 6,
  parameter ADDRESS_RANGE = 64,
  parameter IS_SIMPLE = 0
) (

  // memory port 1
  input wire [ADDRESS_WIDTH-1:0] address0,
  input wire                     ce0,
  input wire [DATA_WIDTH-1:0]    d0,
  input wire                     we0,
  output wire [DATA_WIDTH-1:0]    q0,

  // memory port 2
  input wire [ADDRESS_WIDTH-1:0] address1,
  input wire                    ce1,
  input wire [DATA_WIDTH-1:0]    d1,
  input wire                    we1,
  output wire [DATA_WIDTH-1:0]    q1,
  input wire                    reset,
  input wire                    clk
);
  
  generate
    if (IS_SIMPLE == 1) begin
        memcore_uram_simple #(
           .DATA_WIDTH(DATA_WIDTH),
           .ADDRESS_WIDTH(ADDRESS_WIDTH),
           .ADDRESS_RANGE(ADDRESS_RANGE)
        ) core (
            .clk(clk),
            .reset(reset),
            .address0(address0),
            .ce0(ce0),
            .we0(we0),
            .d0(d0),
            .address1(address1),
            .ce1(ce1),
            .q1(q1)
        );
    end else begin
        memcore_uram_true #(
           .DATA_WIDTH(DATA_WIDTH),
           .ADDRESS_WIDTH(ADDRESS_WIDTH),
           .ADDRESS_RANGE(ADDRESS_RANGE)
        ) core (
            .clk(clk),
            .reset(reset),
            .address0(address0),
            .ce0(ce0),
            .d0(d0),
            .we0(we0),
            .q0(q0),
            .address1(address1),
            .ce1(ce1),
            .d1(d1),
            .we1(we1),
            .q1(q1)
        );
    end
  endgenerate

endmodule

`default_nettype wire
