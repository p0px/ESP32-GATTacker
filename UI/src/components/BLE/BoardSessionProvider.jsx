import { createContext, useContext } from "react";
import { useBoardSession } from "./useBoardSession";

const BoardContext = createContext(null);

export function BoardSessionProvider({ children }) {
  const board = useBoardSession();
  return <BoardContext.Provider value={board}>{children}</BoardContext.Provider>;
}

export function useBoard() {
  const board = useContext(BoardContext);
  if (!board) throw new Error("Board session provider is missing");
  return board;
}
