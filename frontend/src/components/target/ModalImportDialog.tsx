import ImportPreviewPanel from './ImportPreviewPanel';

interface ModalImportDialogProps {
    isOpen: boolean;
    onClose: () => void;
}

const ModalImportDialog: React.FC<ModalImportDialogProps> = ({ isOpen, onClose }) => {
    if (!isOpen) {
        return null;
    }

    const handleOverlayClick = (e: React.MouseEvent) => {
        if (e.target === e.currentTarget) {
            onClose();
        }
    };

    return (
        <div
            style={{
                position: 'fixed',
                top: 0,
                left: 0,
                right: 0,
                bottom: 0,
                backgroundColor: 'rgba(0, 0, 0, 0.5)',
                display: 'flex',
                justifyContent: 'center',
                alignItems: 'center',
                zIndex: 1000,
                opacity: isOpen ? 1 : 0,
                transition: 'opacity 0.3s ease',
            }}
            onClick={handleOverlayClick}
        >
            <div
                style={{
                    background: '#f9f9f9',
                    border: '2px solid #333',
                    borderRadius: '8px',
                    padding: '20px',
                    width: '90%',
                    maxWidth: '1000px',
                    maxHeight: '90vh',
                    overflow: 'auto',
                    transform: isOpen ? 'scale(1)' : 'scale(0.9)',
                    opacity: isOpen ? 1 : 0,
                    transition: 'all 0.3s ease',
                    boxShadow: '0 10px 30px rgba(0, 0, 0, 0.3)',
                }}
            >
                <div style={{
                    display: 'flex',
                    justifyContent: 'space-between',
                    alignItems: 'center',
                    marginBottom: '20px',
                    borderBottom: '1px solid #ddd',
                    paddingBottom: '10px'
                }}>
                    <h2 style={{ margin: 0, fontSize: '18px', color: '#333' }}>数据导入</h2>
                    <button
                        onClick={onClose}
                        style={{
                            background: 'none',
                            border: 'none',
                            fontSize: '24px',
                            cursor: 'pointer',
                            color: '#666',
                            width: '30px',
                            height: '30px',
                            display: 'flex',
                            justifyContent: 'center',
                            alignItems: 'center',
                            borderRadius: '50%',
                            transition: 'background-color 0.2s'
                        }}
                        onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#eee'}
                        onMouseLeave={(e) => e.currentTarget.style.backgroundColor = 'transparent'}
                    >
                        ×
                    </button>
                </div>

                <div style={{ marginBottom: '20px', fontSize: '14px', color: '#666' }}>
                    <p>请通过以下任意一种方式导入目标物数据：</p>
                    <ul style={{ margin: '10px 0', paddingLeft: '20px' }}>
                        <li>在文本框中直接输入或粘贴JSON格式数据</li>
                        <li>点击"选择JSON文件"按钮选择本地JSON文件</li>
                    </ul>
                    <p>数据验证通过后，可以在右侧预览区域查看场景布局。</p>
                </div>

                <ImportPreviewPanel />

                <div style={{
                    display: 'flex',
                    justifyContent: 'flex-end',
                    marginTop: '20px',
                    borderTop: '1px solid #ddd',
                    paddingTop: '15px'
                }}>
                    <button
                        onClick={onClose}
                        style={{
                            padding: '8px 20px',
                            fontSize: '14px',
                            backgroundColor: '#95a5a6',
                            color: 'white',
                            border: 'none',
                            borderRadius: '3px',
                            cursor: 'pointer',
                            transition: 'background-color 0.2s'
                        }}
                        onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#7f8c8d'}
                        onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#95a5a6'}
                    >
                        关闭
                    </button>
                </div>
            </div>
        </div>
    );
};

export default ModalImportDialog;
