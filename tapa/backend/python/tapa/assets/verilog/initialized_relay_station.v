`default_nettype none

module initialized_relay_station #(
    parameter DATA_WIDTH = 32,
    parameter ADDR_WIDTH = 5,
    parameter DEPTH      = 2,
    parameter LEVEL      = 2,
    parameter INIT_LENGTH = 10,
    parameter CONNECT    = 1   // add api to disconnect the relay station
) (
  input wire clk,
  input wire reset,

  // write
  output wire                  if_full_n,
  input  wire                  if_write_ce,
  input  wire                  if_write,
  input  wire [DATA_WIDTH-1:0] if_din,

  // read
  output wire                  if_empty_n,
  input  wire                  if_read_ce,
  input  wire                  if_read,
  output wire [DATA_WIDTH-1:0] if_dout
);

  wire                  full_n  [LEVEL:0];
  wire                  empty_n [LEVEL:0];
  wire [DATA_WIDTH-1:0] data    [LEVEL:0];

  // both full_n and write are registered, thus one level of relay_station cause
  // two additional latency for the almost full fifo
  parameter GRACE_PERIOD = LEVEL * 2;
  parameter REAL_DEPTH = GRACE_PERIOD + DEPTH + 4;
  parameter REAL_ADDR_WIDTH  = $clog2(REAL_DEPTH);

  genvar i;
  generate
  if (CONNECT > 0) begin
    if (LEVEL > 0) begin

      for (i = 0; i < LEVEL; i = i + 1) begin : inst
        if (i < LEVEL - 1) begin
          fifo_reg #(
            .DATA_WIDTH(DATA_WIDTH)
          ) unit (
            .clk(clk),
            .reset(reset),

            // connect to fifo[i+1]
            .if_empty_n(empty_n[i+1]),
            .if_read_ce(if_read_ce),
            .if_read   (full_n[i+1]),
            .if_dout   (data[i+1]),

            // connect to fifo[i-1]
            .if_full_n  (full_n[i]),
            .if_write_ce(if_write_ce),
            .if_write   (empty_n[i]),
            .if_din     (data[i])
          );

        end else begin
          (* keep = "true" *) initialized_almost_full_fifo #(
            .DATA_WIDTH(DATA_WIDTH),
            .ADDR_WIDTH(REAL_ADDR_WIDTH),
            .DEPTH(REAL_DEPTH),
            .GRACE_PERIOD(GRACE_PERIOD),
            .INIT_LENGTH(INIT_LENGTH)
          ) unit (
            .clk(clk),
            .reset(reset),

            // connect to fifo[i+1]
            .if_empty_n(empty_n[i+1]),
            .if_read_ce(if_read_ce),
            .if_read   (full_n[i+1]),
            .if_dout   (data[i+1]),

            // connect to fifo[i-1]
            .if_full_n  (full_n[i]),
            .if_write_ce(if_write_ce),
            .if_write   (empty_n[i]),
            .if_din     (data[i])
          );
        end
      end

      // write
      assign if_full_n  = full_n[0];  // output
      assign empty_n[0] = if_write & full_n[0];   // input
      assign data[0]    = if_din;     // input

      // read
      assign if_empty_n    = empty_n[LEVEL];  // output
      assign full_n[LEVEL] = if_read;         // input
      assign if_dout       = data[LEVEL];     // output

    end

    // LEVEL == 0
    else begin
      assign if_full_n  = if_read;  // output
      assign if_empty_n    = if_write;  // output
      assign if_dout       = if_din;     // output
    end
  end

  // disconnect the relay station
  else begin
    assign if_full_n  = 0;  // output
    assign if_empty_n = 0;  // output
    // leave the dout port dangling to facilitate pruning
    // assign if_dout    = 0;     // output
  end
  endgenerate

endmodule   // relay_station

// first-word fall-through (FWFT) FIFO
module initialized_almost_full_fifo #(
  parameter DATA_WIDTH = 32,
  parameter ADDR_WIDTH = 5,
  parameter DEPTH      = 32,
  parameter GRACE_PERIOD = 2,
  parameter INIT_LENGTH = 32
) (
  input wire clk,
  input wire reset,

  // write
  output wire                  if_full_n,
  input  wire                  if_write_ce,
  input  wire                  if_write,
  input  wire [DATA_WIDTH-1:0] if_din,

  // read
  output wire                  if_empty_n,
  input  wire                  if_read_ce,
  input  wire                  if_read,
  output wire [DATA_WIDTH-1:0] if_dout
);

  // STATE MACHINE DESIGN:
  // STATE_RESET: For as long as reset is kept asserted, the system
  // stays in STATE_RESET. The free_buffers fifo's ports are relayed
  // to the external ports exactly in the same way as in STATE_RELAY
  // Right after reset is asserted low, the state changes to
  // STATE_INIT.
  // STATE_INIT: The free_buffers fifo is initialized with all the
  // partition IDs, cycle by cycle.
  parameter STATE_RESET = 0;
  parameter STATE_INIT = 1;
  parameter STATE_RELAY = 2;

  reg[1:0] state;
  reg[1:0] next_state;
  reg [DATA_WIDTH-1:0] value_to_write;
  wire [DATA_WIDTH-1:0] next_value_to_write;

  // the wires going to free_buffers FIFO are directly
  // connected to the external port in STATE_RELAY and
  // STATE_RESET. However, in STATE_INIT, the external
  // signals are set such that no module will attempt
  // writing/readint this FIFO. Both empty_n and full_n
  // are activated.

  wire internal_fifo_empty_n;
  wire internal_fifo_full_n;
  wire internal_fifo_write_ce;
  wire internal_fifo_write;
  wire [DATA_WIDTH-1:0] internal_fifo_din;
  wire internal_fifo_read_ce;
  wire internal_fifo_read;

  // TODO: Directly keeping write asserted high might be wrong? needs
  // reviewing
  assign if_full_n = (state == STATE_INIT) ? 1 : internal_fifo_full_n;
  assign if_empty_n = (state == STATE_INIT) ? 0 : internal_fifo_empty_n;
  assign internal_fifo_write_ce = (state == STATE_INIT) ? 1 : if_write_ce;
  assign internal_fifo_write = (state == STATE_INIT) ? 1 : if_write;
  assign internal_fifo_din = (state == STATE_INIT) ? value_to_write : if_din;
  assign internal_fifo_read_ce = (state == STATE_INIT) ? 0 : if_read_ce;
  assign internal_fifo_read = (state == STATE_INIT) ? 0 : if_read;

  assign next_value_to_write = value_to_write + 1;

  always @ (posedge clk) begin
    if (reset) begin
      state <= STATE_RESET;
    end else begin
      state <= next_state;
    end
  end

  always @ (posedge clk) begin
    if (reset) begin
      value_to_write <= 0;
    end else begin
      if ((state == STATE_INIT) && (internal_fifo_full_n != 0)) begin
        value_to_write <= next_value_to_write;
      end
    end
  end

  always @ (*) begin
    case (state)
      STATE_RESET: begin
        next_state = STATE_INIT;
      end
      STATE_INIT: begin
        if (value_to_write == (INIT_LENGTH - 1)) begin
          next_state = STATE_RELAY;
        end else begin
          next_state = STATE_INIT;
        end
      end
      STATE_RELAY: begin
        next_state <= STATE_RELAY;
      end
    endcase
  end

  fifo_almost_full #(
    .DATA_WIDTH(DATA_WIDTH),
    .ADDR_WIDTH(ADDR_WIDTH),
    .DEPTH(DEPTH),
    .GRACE_PERIOD(GRACE_PERIOD)
  ) init_fifo (
    .if_full_n(internal_fifo_full_n),
    .if_write_ce(internal_fifo_write_ce),
    .if_write(internal_fifo_write),
    .if_din(internal_fifo_din),
    .if_empty_n(internal_fifo_empty_n),
    .if_read_ce(internal_fifo_read_ce),
    .if_read(internal_fifo_read),
    .if_dout(if_dout),
    .clk(clk),
    .reset(reset)
  );

endmodule

`default_nettype wire
