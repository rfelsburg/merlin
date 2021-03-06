# Many test cases doesn't care about handshaking, but just that the correct
# nodes is configured and connected correctly. For those cases, this step just
# sets everything up according to the structure specified in the table.

Given(/^I start naemon with merlin nodes connected$/) do |nodes|
  sut_portnum = 7000

  configured_peers = 0
  configured_pollers = 0
  configured_masters = 0

  nodes.hashes.each do |obj|
    case obj["type"]
    when "peer"
      configured_peers += 1
    when "poller"
      configured_pollers += 1
    when "master"
      configured_masters += 1
    end
  end

  step "I have merlin configured for port #{sut_portnum}", nodes
  step "ipc listens for merlin at socket test_ipc.sock"
  step "I start naemon"
  step "I wait for 1 second"
  step "ipc received event CTRL_ACTIVE"

  # Just to clean up output, except from initial CTRL_ACTIVE
  step "node ipc have info hash my_hash at 5000"
  step "node ipc have expected hash my_hash at 5000"

  nodes.hashes.each do |obj|
    step "node #{obj["name"]} have info hash my_hash at 3000"
    step "node #{obj["name"]} have expected hash my_hash at 4000"
    step "#{obj["name"]} connect to merlin at port #{sut_portnum} from port #{obj["port"].to_i+sut_portnum}"
    case obj["type"]
    when "peer"
      step "#{obj["name"]} sends event CTRL_ACTIVE", Cucumber::Ast::Table.new([
        ["configured_peers", configured_peers.to_s],
        ["configured_pollers", configured_pollers.to_s],
        ["configured_masters", configured_masters.to_s]
      ])
    when "poller"
      # TODO: multiple poller groups, this assumes a single poller group
      step "#{obj["name"]} sends event CTRL_ACTIVE", Cucumber::Ast::Table.new([
        ["configured_peers", (configured_pollers-1).to_s],
        ["configured_pollers", "0"],
        ["configured_masters", (configured_peers+1).to_s]
      ])
    when "master"
      # TODO: This makes no sense. The master should send the shape as the poller sees it.
      step "#{obj["name"]} sends event CTRL_ACTIVE", Cucumber::Ast::Table.new([
        ["configured_peers", 0],
        ["configured_pollers", 0],
        ["configured_masters", 0]
      ])
    end
    step "#{obj["name"]} received event CTRL_ACTIVE"
    step "#{obj["name"]} is connected to merlin"
  end
end