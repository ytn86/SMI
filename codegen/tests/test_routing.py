import networkx

from program import ProgramMapping, Program
from routing import load_inter_fpga_connections, create_routing_context


def test_load_inter_fpga_connections():
    program = Program([])
    mapping = ProgramMapping([program], {
        "n1:f1": program,
        "n1:f2": program,
        "n2:f1": program
    })

    connections = {
        ("n1:f1", 0): ("n1:f2", 0),
        ("n1:f2", 1): ("n2:f1", 1),
        ("n2:f1", 0): ("n1:f1", 1),
    }

    graph = networkx.Graph()
    fpgas = load_inter_fpga_connections(graph, connections, mapping)

    assert len(fpgas) == 3
    fpgas = sorted(fpgas, key=lambda f: f.key())
    assert fpgas[0].program is program

    assert list(graph.edges(fpgas[0].channels[0])) == [(fpgas[0].channels[0], fpgas[1].channels[0])]
    assert list(graph.edges(fpgas[0].channels[1])) == [(fpgas[0].channels[1], fpgas[2].channels[0])]
    assert list(graph.edges(fpgas[1].channels[1])) == [(fpgas[1].channels[1], fpgas[2].channels[1])]
    assert list(graph.edges(fpgas[2].channels[0])) == [(fpgas[2].channels[0], fpgas[0].channels[1])]


def test_routing_context():
    program = Program([])
    mapping = ProgramMapping([program], {
        "n1:f1": program,
        "n1:f2": program,
        "n2:f1": program,
        "n3:f1": program
    })

    connections = {
        ("n1:f1", 0): ("n1:f2", 0),
        ("n1:f2", 1): ("n2:f1", 1),
        ("n1:f2", 2): ("n3:f1", 1),
        ("n2:f1", 0): ("n1:f1", 1),
    }

    ctx = create_routing_context(connections, mapping)
    fpgas = ctx.fpgas
    assert ctx.routes[fpgas[0].channels[0]][fpgas[3].channels[3]] == [
        fpgas[0].channels[0],
        fpgas[1].channels[0],
        fpgas[1].channels[2],
        fpgas[3].channels[1],
        fpgas[3].channels[3]
    ]
