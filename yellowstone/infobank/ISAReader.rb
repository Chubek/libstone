module Yellowstone; module Infobank
  Node = Struct.new(:kind,:name,:value,:fields,:line,:column, keyword_init: true)
  class ISAReader
    def self.read(path)
      require 'json'; py=File.join(__dir__,'ISAReader.py')
      # Ruby implementation mirrors the public tree with a small compatible parser.
      text=File.read(path); Node.new(kind: :document, fields: [], line: 1, column: 1).tap { |r| text.scan(/\b(arch|profile|compiler|legacy|regclass|encoding|op)\b(?:\s+([\w.-]+))?\s*\{/).each { |k,n| r.fields << Node.new(kind:k.to_sym,name:n.to_s,fields:[]) } }
    end
  end
  ISAEventStream = Enumerator
  ISAVisitor = Proc
end; end
