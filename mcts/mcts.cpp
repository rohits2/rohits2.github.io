#include "mcts.h"

const float C = 1.44;
const float TIE_REWARD = 0.5;
const float inf = std::numeric_limits<float>::infinity();

// Given a board and a parent, returns the node for the board and parent.
// If the board state already exists in the transposition table, this will add the parent to the node.
// If it does not, it will allocate a new node and parent.
// The returned node will be bound to the lifetime of its parent.
shared_ptr<MCTSNode> MCTSTree::get_node(const Board &new_board, shared_ptr<MCTSNode> new_parent) {
    tree_lock.lock();
    total_lookups++;
    if (transposition_table.find(new_board) != transposition_table.end()) {
        auto wk_node = transposition_table[new_board];
        if (wk_node.expired()) {
            transposition_table.erase(transposition_table.find(new_board));
            printf("Found dead node in get_node!\n");
            return get_node(new_board, new_parent);
        }
        shared_ptr<MCTSNode> node = wk_node.lock();
        if (node->parents.size() == 0 && new_parent != nullptr) {
            printf("Unrooting!\n");
            auto itr = find(roots.begin(), roots.end(), node);
            roots.erase(itr);
        }
        if (new_parent != nullptr) {
            node->parents.push_back(new_parent);
        }
        tree_lock.unlock();
        total_hits++;
        return node;
    }
    shared_ptr<MCTSNode> node = make_shared<MCTSNode>(new_board, new_parent, this);
    auto entry = pair<Board, weak_ptr<MCTSNode>>(new_board, node);
    transposition_table.insert(entry);
    if (new_parent == nullptr) {
        printf("Rooting node!\n");
        roots.push_back(node);
    }
    tree_lock.unlock();
    return node;
}

// Commit filicide on all but the most explored child nodes.
// The idea is that we no longer need all of the subtrees from this node,
// only the most common one and the information to seek it out.
// See MCTSNode::filicide to understand how filicide works.
void MCTSTree::prune(unsigned max_size) {
    tree_lock.lock();
    queue<shared_ptr<MCTSNode>> inspection_queue;
    for (shared_ptr<MCTSNode> root : roots) {
        inspection_queue.push(root);
    }
    while (transposition_table.size() > max_size) {
        shared_ptr<MCTSNode> node = inspection_queue.front();
        inspection_queue.pop();
        unsigned max_visits = 0;
        for (auto child : node->children) {
            max_visits = max_visits > node->visits ? max_visits : node->visits;
        }
        for (auto child : node->children) {
            if (child->visits <= max_visits) {
                child->filicide();
            }
        }
    }
    tree_lock.unlock();
}

// Get the percentage of get_node that falls into the transposition table.
float MCTSTree::transposition_hitrate() { return total_hits / ((float)total_lookups); }

// Get the number of nodes in the transposition table
int MCTSTree::transposition_size() { return transposition_table.size(); }

// Get the total number of times filicide() has been invoked
long long MCTSTree::purges() { return total_fillicides; }

// Construct a new MCTSNode - don't use this.
MCTSNode::MCTSNode(const Board &new_board, shared_ptr<MCTSNode> new_parent, MCTSTree *host) {
    board = new_board;
    tree = host;
    parents.push_back(new_parent);
    moves = board.get_valid_moves();
}

// Get the node's expected value (Q-score).
// This is calculated taking ties into account,
// and relies on the current 
float MCTSNode::Q() {
    lock.lock(); //
    float sum = wins + TIE_REWARD * ties;
    float res = sum / (1.0f + visits);
    lock.unlock();
    return res;
}

// Get the parent node's Q-score
float MCTSNode::parent_Q() {
    unsigned losses = visits - wins - ties;
    float loss = losses / (1.0f + visits);
    float tie = TIE_REWARD * ties / (1.0f + visits);
    return loss + tie;
}

float MCTSNode::U() {
    unsigned parent_visit_count = 0;
    for (int i = 0; i < parents.size(); i++) {
        auto parent = parents[i];
        if (parent.expired()) {
            parents.erase(parents.begin() + i);
            i--;
            continue;
        }
        parent_visit_count += parent.lock()->visits;
    }
    return C * sqrt((float)parent_visit_count) / (1.0 + visits);
}

float MCTSNode::PUCT() { return Q() + U(); }

grid_coord MCTSNode::get_move() const {
    float best_Q = inf;
    int best_visits = 0;
    grid_coord best_move = {-1, -1, -1, -1};
    if (!expanded) {
        return best_move;
    }
    lock.lock();
    printf("--- Move enumeration ---\n");
    for (int i = 0; i < children.size(); i++) {
        shared_ptr<MCTSNode> child = children[i];
        float Q = child->Q();
        printf("N(%d, %d, %d, %d)/%d - valued by %d as %f \n ", moves[i].m_i, moves[i].m_j, moves[i].i, moves[i].j,
               child->visits, child->board.player, Q);
        if (Q < best_Q) {
            best_Q = Q;
            best_visits = child->visits;
            best_move = moves[i];
        } else if (Q == best_Q && child->visits > best_visits) {
            best_Q = Q;
            best_visits = child->visits;
            best_move = moves[i];
        }
    }
    printf("----\n");
    lock.unlock();
    return best_move;
}

policy_vec MCTSNode::get_policy() const {
    policy_vec vec;
    if (!expanded) {
        return vec;
    }
    lock.lock();
    for (int ind = 0; ind < children.size(); ind++) {
        shared_ptr<MCTSNode> child = children[ind];
        int i = moves[ind].m_i * 3 + moves[ind].i;
        int j = moves[ind].m_i * 3 + moves[ind].i;
        vec.policy[i][j] = 1 - child->Q() + 0.00001;
    }
    lock.unlock();
    return vec;
}

shared_ptr<MCTSNode> MCTSNode::max_PUCT() {
    float best_PUCT = -inf;
    shared_ptr<MCTSNode> best_node = nullptr;
    lock.lock();
    for (shared_ptr<MCTSNode> child : children) {
        float PUCT = (1 - child->Q()) + child->U();
        if (PUCT > best_PUCT) {
            best_PUCT = PUCT;
            best_node = child;
        }
    }
    lock.unlock();
    return best_node;
}

vector<shared_ptr<MCTSNode>> MCTSNode::select() {
    vector<shared_ptr<MCTSNode>> path;
    path.reserve(64);
    shared_ptr<MCTSNode> cur_node = shared_from_this();
    while (cur_node->expanded) {
        path.push_back(cur_node);
        shared_ptr<MCTSNode> new_node = cur_node->max_PUCT();
        cur_node->lock.lock();
        cur_node->visits++;
        cur_node->lock.unlock();
        cur_node = new_node;
    };
    path.push_back(cur_node);
    cur_node->visits++;
    return path;
}

void MCTSNode::prune_ancestors() { prune_ancestors(shared_from_this()); }
void MCTSNode::prune_children() {
    lock.lock();
    vector<float> Qs;
    for (auto child : children) {
        Qs.push_back(child->Q());
    }
    for (int i = 0; i < children.size(); i++) {
        auto child = children[i];
        float QU = child->Q() + child->U();
        bool prunable = false;
        for (int j = 0; j < i; j++) {
            if (QU < Qs[j]) {
                prunable = true;
            }
        }
        for (int j = i + 1; j < children.size(); j++) {
            if (QU < Qs[j]) {
                prunable = true;
            }
        }
        if (prunable) {
            child->filicide();
        }
    }
    lock.unlock();
}

void MCTSNode::filicide() {
    lock.lock();
    if (!expanded) {
        lock.unlock();
        return;
    }
    children.clear();
    expanded = false;
    lock.unlock();
}

void MCTSNode::prune_ancestors(shared_ptr<MCTSNode> node_to_keep) {
    lock.lock();
    if (shared_from_this() != node_to_keep) {
        for (shared_ptr<MCTSNode> child : children) {
            if (child == node_to_keep) {
                continue;
            }
            child->filicide();
        }
    }
    lock.unlock();
    for (int i = 0; i < parents.size(); i++) {
        auto wk_parent = parents[i];
        if (wk_parent.expired()) {
            parents.erase(parents.begin() + i);
            i--;
            continue;
        }
        shared_ptr<MCTSNode> parent = wk_parent.lock();
        parent->prune_ancestors(shared_from_this());
    }
}

void MCTSNode::expand() {
    lock.lock();
    visits++;
    if (expanded) {
        lock.unlock();
        return;
    }
    for (grid_coord move : moves) {
        expanded = true;
        Board new_board(board);
        new_board.move(move);
        shared_ptr<MCTSNode> new_node = tree->get_node(new_board, shared_from_this());
        children.push_back(new_node);
    }
    lock.unlock();
}

void MCTSNode::backpropagate(const Board &board, vector<shared_ptr<MCTSNode>> path) {
    int winner = board.game_winner();
    for (shared_ptr<MCTSNode> &node : path) {
        node->lock.lock();
        if (winner == node->board.player) {
            node->wins += 1;
        } else if (winner == PLAYER_TIE) {
            node->ties += 1;
        }
        node->lock.unlock();
    }
}

Board simulate(const Board &board) {
    Board new_board(board);
    while (new_board.game_winner() == PLAYER_NONE) {
        vector<grid_coord> s_moves = new_board.get_valid_moves();
        int rnum = rand() % (int)s_moves.size();
        grid_coord move = s_moves[rnum];
        new_board.move(move);
    }
    return new_board;
}

MCTSNode::~MCTSNode() {
    tree->total_fillicides++;
    tree->transposition_table.erase(tree->transposition_table.find(board));
}

void MCTSTree::mcts(const Board &board, int num_iterations) {
    shared_ptr<MCTSNode> node = get_node(board, nullptr);
    for (int it = 0; it < num_iterations; it++) {
        vector<shared_ptr<MCTSNode>> path = node->select();
        shared_ptr<MCTSNode> leaf = path.back();
        auto board = simulate(leaf->board);
        leaf->backpropagate(board, path);
        if (leaf->board.game_winner() == PLAYER_NONE) {
            leaf->expand();
        }
    }
}
/**void MCTSTree::parallel_mcts(const Board &board, int num_iterations) {
    shared_ptr<MCTSNode> node = get_node(board, nullptr);
    int remaining = num_iterations;
    int n_threads = thread::hardware_concurrency() - 1;
    n_threads = n_threads < 1 ? 1 : n_threads;
    int block_sz = num_iterations / n_threads;
    vector<thread *> workers;
    while (remaining > 0) {
        int block = min(block_sz, remaining);
        workers.push_back(new thread(mcts, node, block));
        remaining -= block;
    }
    for (thread *worker : workers) {
        worker->join();
        delete worker;
    }
}**/